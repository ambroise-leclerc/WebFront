#include "../driver/WfDriver.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
struct DriverUI {
    std::vector<std::string>& scripts;
    void                      addScript(std::string_view script) const {
        scripts.emplace_back(script);
    }
};
}  // namespace

SCENARIO("The driver deadline closes both connected and late browsers without losing the request") {
    std::vector<std::string>                      scripts;
    webfront::testdriver::DriverBrowser<DriverUI> browser;
    GIVEN("the browser connects before the deadline") {
        browser.connected(DriverUI{scripts});
        REQUIRE(scripts == std::vector<std::string>{webfront::testdriver::driverScript});
        WHEN("the watchdog closes it twice") {
            browser.close();
            browser.close();
            THEN("one close follows the command runner") {
                REQUIRE(scripts.size() == 2);
                REQUIRE(scripts.back() == "window.close()");
            }
        }
    }
    GIVEN("the deadline expires before a browser connects") {
        browser.close();
        REQUIRE(scripts.empty());
        WHEN("the handshake arrives afterward") {
            browser.connected(DriverUI{scripts});
            THEN("the page closes without starting the command runner") {
                REQUIRE(scripts == std::vector<std::string>{"window.close()"});
            }
        }
    }
}

SCENARIO("Driver WebView2 profiles isolate environments served at identical URLs") {
    using namespace webfront;
    STATIC_REQUIRE(
        std::is_same_v<testdriver::DriverFrontend<frontend::WebView2Frontend>, frontend::WebView2FrontendWithOptions<testdriver::DriverWebView2Options>>);
    STATIC_REQUIRE(std::is_same_v<testdriver::DriverFrontend<frontend::CEFFrontend>, testdriver::DriverCEFFrontend>);
    const auto first  = testdriver::DriverWebView2Options::get("first-session");
    const auto second = testdriver::DriverWebView2Options::get("second-session");
    REQUIRE(first.userDataFolder == std::filesystem::path("first-session"));
    REQUIRE(second.userDataFolder != first.userDataFolder);
    REQUIRE(testdriver::DriverWebView2Options::get(nullptr).userDataFolder.empty());
    REQUIRE(testdriver::DriverWebView2Options::get("").userDataFolder.empty());
}

SCENARIO("Driver suites validate names, commands and filters before opening a browser") {
    using namespace webfront::testdriver;
    const auto source = R"([{"name":"install","command":["install","a b"]},{"name":"failure","expect":"1","command":["nosuchcommand"]}])";
    const auto cases  = parseSuite(source);
    REQUIRE(cases.size() == 2);
    REQUIRE(cases[0].command == std::vector<std::string>{"install", "a b"});
    REQUIRE(cases[1].expected == 1);
    REQUIRE(parseSuite(source, "fail").front().name == "failure");
    REQUIRE_THROWS(parseSuite(source, "absent"));
    for (const auto invalid : {"[]",
                               R"([{"name":"../escape","command":["list"]}])",
                               R"([{"name":"x","command":[]}])",
                               R"([{"name":"x","command":["list"],"expect":"-1"}])",
                               R"([{"name":"x","command":["list"]},{"name":"x","command":["list"]}])",
                               R"([{"name":"x","command":["list"],"extra":true}])",
                               "[] trailing"})
        REQUIRE_THROWS(parseSuite(invalid));
}

SCENARIO("Suite watchdogs keep a native per-case limit inside the overall budget") {
    using namespace std::chrono_literals;
    using webfront::testdriver::DriverDeadline;
    DriverDeadline deadline{1h};
    deadline.beginCase(1ms);
    std::stop_source fallback;
    std::jthread     cancelIfBroken([&](std::stop_token stop) {
        std::mutex                  mutex;
        std::condition_variable_any wake;
        std::unique_lock            lock{mutex};
        if (!wake.wait_for(lock,
                           stop,
                           10s,
                           [] {
                               return false;
                           })
            && !stop.stop_requested())
            fallback.request_stop();
    });
    REQUIRE(deadline.expired(fallback.get_token()));
    cancelIfBroken.request_stop();
    deadline.endCase();
    std::stop_source cancelled;
    cancelled.request_stop();
    REQUIRE_FALSE(deadline.expired(cancelled.get_token()));
    REQUIRE(webfront::testdriver::parseSuite(R"([{"name":"x","command":["list"],"timeout":"17"}])").front().timeout == 17s);
    REQUIRE_THROWS(webfront::testdriver::parseSuite(R"([{"name":"x","command":["list"],"timeout":"0"}])"));
}
