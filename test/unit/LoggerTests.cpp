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

SCENARIO("A second removal of the same fallback sink also waits for the active delivery (#542)") {
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
    std::promise<void> firstRemoved, secondRemoved;
    auto               firstDone  = firstRemoved.get_future();
    auto               secondDone = secondRemoved.get_future();
    std::thread        first([&] {
        log::removeSinks(sink);
        firstRemoved.set_value();
    });
    // The slot is emptied at once, while the first removal keeps waiting for the delivery.
    if (running)
        CHECK(firstDone.wait_for(50ms) == std::future_status::timeout);
    std::thread second([&] {
        log::removeSinks(sink);
        secondRemoved.set_value();
    });
    // A negative observation of a deliberately blocked call, not a readiness deadline.
    if (running)
        CHECK(secondDone.wait_for(50ms) == std::future_status::timeout);
    release.set_value();
    producer.join();
    first.join();
    second.join();
    CHECK(firstDone.wait_for(0ms) == std::future_status::ready);
    CHECK(secondDone.wait_for(0ms) == std::future_status::ready);
    // Once nothing runs the callback, a further removal returns at once.
    log::removeSinks(sink);
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
            CHECK_FALSE(isRequestLine("[I] Call 12 to cppFunction [W]"));
        }
    }
    GIVEN("the rows of a hex dump, which carry no level tag") {
        THEN("they are left out") {
            CHECK_FALSE(isRequestLine("00000010  5b 57 5d 20 20 20 20 20  20 20 20 20 20 20 20 20  |[W]             |"));
            CHECK_FALSE(isRequestLine("00000000  47 45 54 20 2f 20 48 54  54 50 2f 31 2e 31 0d 0a  |GET / HTTP/1.1..|"));
        }
    }
}

#if !defined(WEBFRONT_USE_MDDLOG) || !WEBFRONT_USE_MDDLOG
SCENARIO("Fallback sinks reuse their slots and a stale id cannot remove another sink") {
    log::setLogLevel(log::Info);
    GIVEN("a sink that was removed and a new one that took its slot") {
        std::atomic<unsigned> first{0}, second{0};
        const auto            stale = log::addSinks([&](std::string_view) { ++first; });
        log::removeSinks(stale);
        const auto live = log::addSinks([&](std::string_view) { ++second; });
        THEN("the slot is reused with another generation") {
            CHECK(live.index == stale.index);
            CHECK(live.generation != stale.generation);
        }
        WHEN("the stale id is removed again") {
            log::removeSinks(stale);
            log::info("still delivered");
            THEN("the new sink keeps receiving lines") {
                CHECK(second == 1);
                CHECK(first == 0);
            }
        }
        log::removeSinks(live);
    }
    GIVEN("a hundred thousand transient sinks") {
        const auto before = log::addSinks([](std::string_view) {});
        std::size_t highest = 0;
        for (int i = 0; i < 100000; ++i) {
            const auto id = log::addSinks([](std::string_view) {});
            highest       = std::max(highest, id.index);
            log::removeSinks(id);
        }
        THEN("the table stays as small as the sinks alive") {
            CHECK(highest < 16);
        }
        log::removeSinks(before);
    }
    GIVEN("two callbacks registered at once") {
        std::atomic<unsigned> a{0}, b{0};
        const auto            ids = log::addSinks([&](std::string_view) { ++a; }, [&](std::string_view) { ++b; });
        THEN("both can be removed") {
            log::removeSinks(ids[0]);
            log::info("one left");
            CHECK(a == 0);
            CHECK(b == 1);
            log::removeSinks(ids[1]);
            log::info("none left");
            CHECK(b == 1);
        }
    }
    log::setLogLevel(log::Error);
}

SCENARIO("A sink removed from another sink's callback waits for that sink's delivery on another thread") {
    using namespace std::chrono_literals;
    log::setLogLevel(log::Info);
    std::promise<void>    entered, release;
    auto                  ready = entered.get_future();
    const auto            gate  = release.get_future().share();
    std::atomic<bool>     finished{false}, victimGone{false};
    const auto            victim = log::addSinks([&](std::string_view line) {
        if (line.find("victim") == std::string_view::npos)
            return;
        entered.set_value();
        gate.wait();
        std::this_thread::sleep_for(20ms);
        finished = true;
    });
    std::promise<void>    removed;
    const auto            remover = log::addSinks([&](std::string_view line) {
        if (line.find("remove") == std::string_view::npos)
            return;
        log::removeSinks(victim);
        victimGone = finished.load();  // the other thread's delivery is over when the removal returns
        removed.set_value();
    });
    std::thread           slow([] { log::info("victim"); });
    const bool            running = ready.wait_for(30s) == std::future_status::ready;
    REQUIRE(running);
    std::thread           removing([] { log::info("remove"); });
    CHECK(removed.get_future().wait_for(50ms) == std::future_status::timeout);
    release.set_value();
    slow.join();
    removing.join();
    CHECK(victimGone);
    log::removeSinks(remover);
    log::setLogLevel(log::Error);
}

SCENARIO("Two sinks that remove each other from their callbacks do not deadlock") {
    using namespace std::chrono_literals;
    log::setLogLevel(log::Info);
    std::promise<void> aIn, bIn;
    auto               aReady = aIn.get_future();
    auto               bReady = bIn.get_future();
    std::promise<void> go;
    const auto         start = go.get_future().share();
    log::SinkId        a, b;
    std::atomic<int>   returned{0};
    std::atomic<bool>  idA{false}, idB{false};
    a = log::addSinks([&](std::string_view line) {
        if (line.find("both") == std::string_view::npos || idA.exchange(true))
            return;
        aIn.set_value();
        start.wait();
        log::removeSinks(b);
        ++returned;
    });
    b = log::addSinks([&](std::string_view line) {
        if (line.find("both") == std::string_view::npos || idB.exchange(true))
            return;
        bIn.set_value();
        start.wait();
        log::removeSinks(a);
        ++returned;
    });
    std::thread one([] { log::info("both"); });
    std::thread two([] { log::info("both"); });
    const bool  running = aReady.wait_for(30s) == std::future_status::ready && bReady.wait_for(30s) == std::future_status::ready;
    go.set_value();
    one.join();
    two.join();
    CHECK(running);
    CHECK(returned == 2);
    log::removeSinks(a, b);
    log::setLogLevel(log::Error);
}
#endif
