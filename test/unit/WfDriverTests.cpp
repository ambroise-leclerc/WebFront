#include "../driver/WfDriver.hpp"
#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include <atomic>

namespace {
webfront::testdriver::Options parseDriver(std::initializer_list<std::string> extra) {
    std::vector<std::string> arguments{"webfront-wf", "--project", "project", "--environment", "environment.tgz", "--integrity", "sha512-test"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return webfront::testdriver::parse(static_cast<int>(argv.size()), argv.data());
}
}  // namespace

SCENARIO("Browser test diagnostics retain HTTP failures without filling the transcript with bridge traffic") {
    std::ostringstream output;
    auto*              previous = std::clog.rdbuf(output.rdbuf());
    const auto         sink     = webfront::testdriver::logRequests();
    webfront::log::info("Received request GET on /scripts.js");
    webfront::log::info("  responding http 404");
    webfront::log::info("onMessage(binary): bridge traffic");
    webfront::log::warn("HTTP asset request failed: denied");
    webfront::log::warn("File response ended its response early: short read");
    webfront::log::warn("WebSocket disconnected unexpectedly");
    webfront::log::removeSinks(sink);
    std::clog.rdbuf(previous);
    webfront::log::setLogLevel(webfront::log::Disabled);
    THEN("the resource, status and failures remain, but ordinary bridge messages do not") {
        CHECK(output.str().find("GET on /scripts.js") != std::string::npos);
        CHECK(output.str().find("responding http 404") != std::string::npos);
        CHECK(output.str().find("request failed: denied") != std::string::npos);
        CHECK(output.str().find("response early: short read") != std::string::npos);
        CHECK(output.str().find("WebSocket disconnected unexpectedly") != std::string::npos);
        CHECK(output.str().find("bridge traffic") == std::string::npos);
    }
}

SCENARIO("The driver repeats its close request until the window has closed") {
    using namespace std::chrono_literals;
    using webfront::testdriver::closeUntilShown;
    std::stop_source shown;
    std::atomic<int> requests = 0;
    GIVEN("a window that only exists after a few requests reached nothing") {
        const auto requestClose = [&] {
            // The third request finds the window; show() then returns.
            if (++requests < 3)
                return false;
            shown.request_stop();
            return true;
        };
        THEN("the request is repeated until show() returns") {
            REQUIRE(closeUntilShown(shown.get_token(), requestClose, 1h));
            REQUIRE(requests == 3);
        }
    }
    GIVEN("a window that never closes") {
        THEN("the driver gives up after its patience") {
            REQUIRE_FALSE(closeUntilShown(
                shown.get_token(),
                [&] {
                    ++requests;
                    return true;
                },
                30ms));
            REQUIRE(requests >= 1);
        }
    }
}

SCENARIO("The page's end releases the driver's close without a time limit") {
    using namespace std::chrono_literals;
    webfront::testdriver::DriverDeadline deadline{1h};
    std::stop_source                     shown;
    std::jthread                         page([&] {
        deadline.finish();
    });
    REQUIRE_FALSE(deadline.expired(shown.get_token()));
    REQUIRE_FALSE(shown.stop_requested());
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

SCENARIO("A single-command watchdog has a separate budget after bootstrap") {
    using namespace std::chrono_literals;
    GIVEN("an overall deadline and a shorter command limit") {
        const auto options = parseDriver({"--timeout", "100", "--command-timeout", "4", "--", "run", "sleep"});
        THEN("both limits survive argument parsing independently") {
            REQUIRE(options.timeout == 100s);
            REQUIRE(options.commandTimeout == 4s);
            REQUIRE(options.command == std::vector<std::string>{"run", "sleep"});
        }
    }
    GIVEN("no separate command limit") {
        REQUIRE_FALSE(parseDriver({"list"}).commandTimeout);
    }
    GIVEN("invalid or ambiguous limits") {
        REQUIRE_THROWS_AS(parseDriver({"--command-timeout", "0", "list"}), webfront::testdriver::UsageError);
        REQUIRE_THROWS_AS(parseDriver({"--command-timeout", "bad", "list"}), webfront::testdriver::UsageError);
    }
}

SCENARIO("The driver watchdog keeps a native command limit inside the overall budget") {
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
    // Starting a command never extends an already expiring overall limit.
    DriverDeadline overall{1ms};
    overall.beginCase(1h);
    REQUIRE(overall.expired(fallback.get_token()));
    cancelIfBroken.request_stop();
    deadline.endCase();
    std::stop_source cancelled;
    cancelled.request_stop();
    REQUIRE_FALSE(deadline.expired(cancelled.get_token()));
}
