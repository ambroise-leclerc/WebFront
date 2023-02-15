#include <tooling/Logger.hpp>

#include <doctest/doctest.h>

#include <list>
#include <string>

using namespace webfront;
using namespace std;

SCENARIO("Logger") {
    std::list<std::string> logs;
    auto testSink = [&logs](std::string_view text) { logs.emplace_back(text); };

    GIVEN("A logger already initialized in main test file") {
        WHEN("A debug and a warn log are issued") {
            auto sinkId = log::addSinks(testSink);
            log::debug("debug log 5");
            log::warn("warn log 6");
            THEN("The precedent configuration has persisted and only warnings are enabled") { REQUIRE(logs.size() == 1); }
            log::removeSinks(sinkId);
        }
    }

    GIVEN("A Logger with a debug level config") {
        log::setLogLevel(log::Debug);
        auto sinkId = log::addSinks(testSink);

        WHEN("An info and debug lines are logged") {
            log::debug("Debug log");
            log::info("Info log");
            THEN("Two logs should have been correctly issued") {
                auto& infoLog = logs.back();
                REQUIRE(infoLog.starts_with("[I]"));
                REQUIRE(infoLog.ends_with("Info log"));

                logs.pop_back();
                auto& debugLog = logs.back();
                REQUIRE(debugLog.starts_with("[D]"));
                REQUIRE(debugLog.ends_with("Debug log"));
#ifndef __llvm__    // No support for source_location filename for now
                REQUIRE(debugLog.find("LoggerTests.cpp") != debugLog.npos);
#endif
            }
        }

        WHEN("Selected log levels are disabled") {
            size_t logCounter = 0;
            log::set(log::Warn, false);
            log::set(log::Error, false);
            log::warn("Warn log:{} enabled [{}]", logCounter++, log::is(log::Warn) ? 'x' : ' ');
            log::info("Info log:{} enabled [{}]", logCounter++, log::is(log::Info) ? 'x' : ' ');
            log::debug("Debug log:{} enabled [{}]", logCounter++, log::is(log::Debug) ? 'x' : ' ');
            log::error("Error log:{} enabled [{}]", logCounter++, log::is(log::Error) ? 'x' : ' ');
            log::set(log::Warn, true);
            log::set(log::Info, false);
            log::set(log::Debug, false);
            log::set(log::Error, true);
            log::warn("Warn log:{} enabled [{}]", logCounter++, log::is(log::Warn) ? 'x' : ' ');
            log::info("Info log:{} enabled [{}]", logCounter++, log::is(log::Info) ? 'x' : ' ');
            log::debug("Debug log:{} enabled [{}]", logCounter++, log::is(log::Debug) ? 'x' : ' ');
            log::error("Error log:{} enabled [{}]", logCounter++, log::is(log::Error) ? 'x' : ' ');
            log::setLogLevel(log::Info);
            log::warn("Warn log:{} enabled [{}]", logCounter++, log::is(log::Warn) ? 'x' : ' ');
            log::info("Info log:{} enabled [{}]", logCounter++, log::is(log::Info) ? 'x' : ' ');
            log::debug("Debug log:{} enabled [{}]", logCounter++, log::is(log::Debug) ? 'x' : ' ');
            log::error("Error log:{} enabled [{}]", logCounter++, log::is(log::Error) ? 'x' : ' ');
            
            THEN("Only enabled level should have produced a log") {
                for (auto& logLine : logs) {
                    std::clog << "    logline : " << logLine << "\n";
                    REQUIRE(logLine.ends_with("enabled [x]"));
                }

                for (auto level : {'I', 'D', 'W', 'E', 'W', 'I', 'E'}) {
                    REQUIRE(logs.front().at(1) == level);
                    logs.pop_front();
                }   
            }
        }

        log::removeSinks(sinkId);
    }
}