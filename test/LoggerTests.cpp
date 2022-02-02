#include <tooling/Logger.hpp>

#include <catch2/catch.hpp>

#include <list>
#include <string>

using namespace webfront;
using namespace std;





SCENARIO("Logger") {

    std::list<std::string> logs;
    auto testSink = [&logs](std::string_view text) {
        logs.emplace_back(text);
    };

    GIVEN("A Logger with a debug level config") {
        log::setLogLevel(log::Debug);
        log::addSinks(log::clogSink, testSink);

        WHEN("An info and debug lines are logged") {
            log::debug("Debug log");
            log::info("Info log");
            THEN("Two logs should have been correctly issued") {
                for (auto l : logs) cout << "Log : " << l << "\n"; 

                auto& infoLog = logs.back();
                REQUIRE(infoLog.starts_with("[I]"));
                REQUIRE(infoLog.ends_with("Info log"));

                logs.pop_back();
                auto& debugLog = logs.back();
                REQUIRE(debugLog.starts_with("[D]"));
                REQUIRE(debugLog.ends_with("Debug log"));
                REQUIRE(debugLog.find("LoggerTests.cpp") != debugLog.npos);

            }
        }
    }

}