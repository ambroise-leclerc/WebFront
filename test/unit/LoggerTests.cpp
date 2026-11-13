#include "../driver/RequestLog.hpp"
#include <webfront/tooling/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <list>
#include <string>
#include <atomic>
#include <future>
#include <thread>

using namespace webfront;
using namespace std;

SCENARIO("Logger") {
    std::list<std::string> logs;
    auto testSink = [&logs](std::string_view text) { logs.emplace_back(text); };

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

#if !defined(WEBFRONT_USE_MDDLOG) || !WEBFRONT_USE_MDDLOG
SCENARIO("Fallback logger removal waits for active delivery") {
    using namespace std::chrono_literals;
    log::setLogLevel(log::Info);
    std::promise<void> entered, release;
    auto               ready = entered.get_future();
    const auto         gate  = release.get_future().share();
    const auto         sink  = log::addSinks([&](std::string_view) {
        entered.set_value();
        gate.wait();
    });
    std::thread        producer([] {
        log::info("blocked delivery");
    });
    const bool         running = ready.wait_for(30s) == std::future_status::ready;
    CHECK(running);
    std::promise<void> removed;
    auto               done = removed.get_future();
    std::thread        remover([&] {
        log::removeSinks(sink);
        removed.set_value();
    });
    // A negative observation of a deliberately blocked call, not a readiness deadline.
    if (running)
        CHECK(done.wait_for(50ms) == std::future_status::timeout);
    release.set_value();
    producer.join();
    remover.join();
    CHECK(done.wait_for(0ms) == std::future_status::ready);
    log::setLogLevel(log::Error);
}

SCENARIO("Fallback logger registration and delivery may run on different threads") {
    log::setLogLevel(log::Info);
    std::atomic<unsigned> seen{0};
    const auto            sink = log::addSinks([&](std::string_view line) {
        if (line.find("stable record") != std::string_view::npos)
            ++seen;
    });
    constexpr unsigned    each = 2000;
    auto                  emit = [] {
        for (unsigned i = 0; i < each; ++i) {
            log::info("stable record");
            log::warn("level changed concurrently");
        }
    };
    std::thread first(emit), second(emit);
    std::thread registrations([] {
        for (unsigned i = 0; i < each; ++i) {
            const auto temporary = log::addSinks([](std::string_view) { });
            log::set(log::Warn, (i & 1) != 0);
            log::removeSinks(temporary);
        }
    });
    first.join();
    second.join();
    registrations.join();
    log::removeSinks(sink);
    log::setLogLevel(log::Error);
    REQUIRE(seen == 2 * each);
}
#endif

SCENARIO("The request log keeps what the HTTP layer says about a request (#542)") {
    using webfront::testdriver::isRequestLine;
    // The wording of HTTPServer.hpp: a renamed message must fail here instead of emptying the transcript of a failed case.
    GIVEN("the lines of an answered request") {
        THEN("they are kept") {
            CHECK(isRequestLine("[I] Received request GET on /WebFront.js"));
            CHECK(isRequestLine("[I]   responding http 200"));
            CHECK(isRequestLine("[W] HTTP asset request failed: boom"));
            CHECK(isRequestLine("[W] Route /api failed: boom"));
            CHECK(isRequestLine("[W] connection ended its response early: boom"));
        }
    }
    GIVEN("bridge traffic logged at the information level") {
        THEN("it is left out") {
            CHECK_FALSE(isRequestLine("[I] Call 12 to cppFunction x"));
        }
    }
}
