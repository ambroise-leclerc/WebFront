#include "../driver/RequestLog.hpp"
#include <webfront/tooling/Logger.hpp>

#include "Bdd.hpp"

#include <list>
#include <string>
#include <atomic>
#include <future>
#include <thread>

using namespace webfront::bdd;

using namespace webfront;
using namespace std;

Scenario loggerScenario{"Logger", [] {
    std::list<std::string> logs;
    auto testSink = [&logs](std::string_view text) { logs.emplace_back(text); };

    given("A Logger with a debug level config", [&] {
        log::setLogLevel(log::Debug);
        auto sinkId = log::addSinks(testSink);

        when("An info and debug lines are logged", [&] {
            log::debug("Debug log");
            log::info("Info log");
            then("Two logs should have been correctly issued", [&] {
                auto& infoLog = logs.back();
                require(infoLog.starts_with("[I]"));
                require(infoLog.ends_with("Info log"));

                logs.pop_back();
                auto& debugLog = logs.back();
                require(debugLog.starts_with("[D]"));
                require(debugLog.ends_with("Debug log"));
#ifndef __llvm__    // No support for source_location filename for now
                require(debugLog.find("LoggerTests.cpp") != debugLog.npos);
#endif
            });
        });

        when("Selected log levels are disabled", [&] {
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
            
            then("Only enabled level should have produced a log", [&] {
                for (auto& logLine : logs) {
                    std::clog << "    logline : " << logLine << "\n";
                    require(logLine.ends_with("enabled [x]"));
                }

                for (auto level : {'I', 'D', 'W', 'E', 'W', 'I', 'E'}) {
                    require(logs.front().at(1) == level);
                    logs.pop_front();
                }   
            });
        });

        log::removeSinks(sinkId);
    });
}};

#if !defined(WEBFRONT_USE_MDDLOG) || !WEBFRONT_USE_MDDLOG
Scenario fallbackLogger{"Fallback logger removal waits for active delivery", [] {
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
    check(running);
    std::promise<void> removed;
    auto               done = removed.get_future();
    std::thread        remover([&] {
        log::removeSinks(sink);
        removed.set_value();
    });
    // A negative observation of a deliberately blocked call, not a readiness deadline.
    if (running)
        check(done.wait_for(50ms) == std::future_status::timeout);
    release.set_value();
    producer.join();
    remover.join();
    check(done.wait_for(0ms) == std::future_status::ready);
    log::setLogLevel(log::Error);
}};

Scenario secondRemoval{"A second removal of the same fallback sink also waits for the active delivery (#542)", [] {
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
    check(running);
    std::promise<void> firstRemoved, secondRemoved;
    auto               firstDone  = firstRemoved.get_future();
    auto               secondDone = secondRemoved.get_future();
    std::thread        first([&] {
        log::removeSinks(sink);
        firstRemoved.set_value();
    });
    // The slot is emptied at once, while the first removal keeps waiting for the delivery.
    if (running)
        check(firstDone.wait_for(50ms) == std::future_status::timeout);
    std::thread second([&] {
        log::removeSinks(sink);
        secondRemoved.set_value();
    });
    // A negative observation of a deliberately blocked call, not a readiness deadline.
    if (running)
        check(secondDone.wait_for(50ms) == std::future_status::timeout);
    release.set_value();
    producer.join();
    first.join();
    second.join();
    check(firstDone.wait_for(0ms) == std::future_status::ready);
    check(secondDone.wait_for(0ms) == std::future_status::ready);
    // Once nothing runs the callback, a further removal returns at once.
    log::removeSinks(sink);
    log::setLogLevel(log::Error);
}};

Scenario fallbackLoggerRegistration{"Fallback logger registration and delivery may run on different threads", [] {
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
    require(seen == 2 * each);
}};
#endif

Scenario requestLog{"The request log keeps what the HTTP layer says about a request (#542)", [] {
    using webfront::testdriver::isRequestLine;
    // The wording of HTTPServer.hpp: a renamed message must fail here instead of emptying the transcript of a failed case.
    given("the lines of an answered request", [&] {
        then("they are kept", [&] {
            check(isRequestLine("[I] Received request GET on /WebFront.js"));
            check(isRequestLine("[I]   responding http 200"));
            check(isRequestLine("[W] HTTP asset request failed: boom"));
            check(isRequestLine("[W] Route /api failed: boom"));
            check(isRequestLine("[W] connection ended its response early: boom"));
        });
    });
    given("bridge traffic logged at the information level", [&] {
        then("it is left out", [&] {
            check(!isRequestLine("[I] Call 12 to cppFunction x"));
        });
    });
}};
