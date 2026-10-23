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
