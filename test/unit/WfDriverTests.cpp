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
    STATIC_REQUIRE(std::is_same_v<testdriver::DriverFrontend<frontend::CEFFrontend>, frontend::CEFFrontend>);
    const auto first  = testdriver::DriverWebView2Options::get("first-session");
    const auto second = testdriver::DriverWebView2Options::get("second-session");
    REQUIRE(first.userDataFolder == std::filesystem::path("first-session"));
    REQUIRE(second.userDataFolder != first.userDataFolder);
    REQUIRE(testdriver::DriverWebView2Options::get(nullptr).userDataFolder.empty());
    REQUIRE(testdriver::DriverWebView2Options::get("").userDataFolder.empty());
}
