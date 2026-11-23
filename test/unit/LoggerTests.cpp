// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-01-29
//
// WebFront — tests/unit
// Role: The logging facilities: levels, sinks, generation-checked identifiers and concurrency.
// Thread: worker-safe | Alloc: unrestricted
// LLM: restores the global level after each scenario

#include "../driver/RequestLog.hpp"
#include <webfront/testing/Bdd.hpp>
#include <webfront/tooling/Logger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <string>
#include <thread>
#include <utility>

using namespace webfront::bdd;

using namespace webfront;
using namespace std;

const Scenario loggerScenario{"Logger", [] {
                                  std::list<std::string> logs;
                                  auto                   testSink = [&logs](std::string_view text) {
                                      logs.emplace_back(text);
                                  };

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
#ifndef __llvm__  // No support for source_location filename for now
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
const Scenario fallbackLogger{"Fallback logger removal waits for active delivery", [] {
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
                                  const bool         running = waitReady(ready);
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

const Scenario secondRemoval{"A second removal of the same fallback sink also waits for the active delivery (#542)", [] {
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
                                 const bool         running = checkReady(ready);
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

const Scenario fallbackLoggerRegistration{"Fallback logger registration and delivery may run on different threads", [] {
                                              log::setLogLevel(log::Info);
                                              std::atomic<unsigned>     seen{0};
                                              const auto                sink = log::addSinks([&](std::string_view line) {
                                                  if (line.find("stable record") != std::string_view::npos)
                                                      ++seen;
                                              });
                                              static constexpr unsigned each = 2000;
                                              auto                      emit = [] {
                                                  for (unsigned i = 0; i < each; ++i) {
                                                      log::info("stable record");
                                                      log::warn("level changed concurrently");
                                                  }
                                              };
                                              std::thread first(emit), second(emit);
                                              std::thread registrations([] {
                                                  for (unsigned i = 0; i < each; ++i) {
                                                      const auto temporary = log::addSinks([](std::string_view) {});
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

namespace {
// Restores the level that the other scenarios expect, on every path.
struct LevelRestore {
    ~LevelRestore() {
        log::setLogLevel(log::Error);
    }
};
// Opens a gate once, however the scenario ends, so that the threads blocked on it can be joined.
struct Gate {
    std::promise<void>       promise;
    std::shared_future<void> future{promise.get_future().share()};
    bool                     opened = false;
    void                     open() {
        if (!std::exchange(opened, true))
            promise.set_value();
    }
    ~Gate() {
        open();
    }
};
}  // namespace

const Scenario sinkSlots{"Fallback sinks reuse their slots and a stale id cannot remove another sink", [] {
                             const LevelRestore restore;
                             log::setLogLevel(log::Info);
                             given("a sink that was removed and a new one that took its slot", [&] {
                                 std::atomic<unsigned> first{0}, second{0};
                                 const auto            stale = log::addSinks([&](std::string_view) {
                                     ++first;
                                 });
                                 log::removeSinks(stale);
                                 const auto live = log::addSinks([&](std::string_view) {
                                     ++second;
                                 });
                                 then("the slot is reused with another generation", [&] {
                                     check(live.index == stale.index);
                                     check(live.generation != stale.generation);
                                 });
                                 when("the stale id is removed again", [&] {
                                     log::removeSinks(stale);
                                     log::info("still delivered");
                                     then("the new sink keeps receiving lines", [&] {
                                         check(second == 1);
                                         check(first == 0);
                                     });
                                 });
                                 log::removeSinks(live);
                             });
                             given("a hundred thousand transient sinks", [&] {
                                 const auto  before  = log::addSinks([](std::string_view) {});
                                 std::size_t highest = 0;
                                 for (int i = 0; i < 100000; ++i) {
                                     const auto id = log::addSinks([](std::string_view) {});
                                     highest       = std::max(highest, id.index);
                                     log::removeSinks(id);
                                 }
                                 then("the table stays as small as the sinks alive", [&] {
                                     // Sinks of other scenarios may still be registered; the bound is far below the number added.
                                     check(highest < 64);
                                 });
                                 log::removeSinks(before);
                             });
                             given("two callbacks registered at once", [&] {
                                 std::atomic<unsigned> a{0}, b{0};
                                 const auto            ids = log::addSinks(
                                     [&](std::string_view) {
                                         ++a;
                                     },
                                     [&](std::string_view) {
                                         ++b;
                                     });
                                 then("both can be removed one by one", [&] {
                                     log::removeSinks(ids[0]);
                                     log::info("one left");
                                     check(a == 0);
                                     check(b == 1);
                                     log::removeSinks(ids[1]);
                                     log::info("none left");
                                     check(b == 1);
                                 });
                             });
                         }};

const Scenario removalFromCallback{"A sink removed from another sink's callback waits for that sink's delivery on another thread", [] {
                                       using namespace std::chrono_literals;
                                       const LevelRestore restore;
                                       log::setLogLevel(log::Info);
                                       std::promise<void> entered, removed;
                                       auto               ready = entered.get_future();
                                       std::atomic<bool>  finished{false}, victimGone{false};
                                       Gate               gate;
                                       const auto         victim      = log::addSinks([&](std::string_view line) {
                                           if (line.find("victim") == std::string_view::npos)
                                               return;
                                           entered.set_value();
                                           gate.future.wait();
                                           std::this_thread::sleep_for(20ms);
                                           finished = true;
                                       });
                                       const auto         remover     = log::addSinks([&](std::string_view line) {
                                           if (line.find("remove") == std::string_view::npos)
                                               return;
                                           log::removeSinks(victim);
                                           victimGone = finished.load();  // the other thread's delivery is over when the removal returns
                                           removed.set_value();
                                       });
                                       auto               removedDone = removed.get_future();
                                       std::jthread       slow([] {
                                           log::info("victim");
                                       });
                                       std::jthread       removing;
                                       const bool         running = waitReady(ready);
                                       check(running);
                                       if (running) {
                                           removing = std::jthread([] {
                                               log::info("remove");
                                           });
                                           // A negative observation of a deliberately blocked call, not a readiness deadline.
                                           check(removedDone.wait_for(50ms) == std::future_status::timeout);
                                       }
                                       gate.open();
                                       slow.join();
                                       if (removing.joinable())
                                           removing.join();
                                       check(victimGone.load());
                                       log::removeSinks(remover);
                                   }};

const Scenario mutualRemoval{"Two sinks that remove each other from their callbacks do not deadlock", [] {
                                 const LevelRestore restore;
                                 log::setLogLevel(log::Info);
                                 std::promise<void> aIn, bIn;
                                 auto               aReady = aIn.get_future();
                                 auto               bReady = bIn.get_future();
                                 Gate               start;
                                 log::SinkId        a, b;
                                 std::atomic<int>   returned{0};
                                 std::atomic<bool>  inA{false}, inB{false};
                                 a = log::addSinks([&](std::string_view line) {
                                     if (line.find("both") == std::string_view::npos || inA.exchange(true))
                                         return;
                                     aIn.set_value();
                                     start.future.wait();
                                     log::removeSinks(b);
                                     ++returned;
                                 });
                                 b = log::addSinks([&](std::string_view line) {
                                     if (line.find("both") == std::string_view::npos || inB.exchange(true))
                                         return;
                                     bIn.set_value();
                                     start.future.wait();
                                     log::removeSinks(a);
                                     ++returned;
                                 });
                                 std::jthread one([] {
                                     log::info("both");
                                 });
                                 std::jthread two([] {
                                     log::info("both");
                                 });
                                 const bool   running = waitReady(aReady) && waitReady(bReady);
                                 check(running);
                                 start.open();
                                 one.join();
                                 two.join();
                                 check(returned == 2);
                                 log::removeSinks(a, b);
                             }};
#endif

const Scenario requestLog{"The request log keeps what the HTTP layer says about a request (#542)", [] {
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
                                      check(!isRequestLine("[I] Call 12 to cppFunction [W]"));
                                  });
                              });
                              given("a warning whose message spans several lines", [&] {
                                  then("it is kept whole, as the sink receives it in one call", [&] {
                                      check(isRequestLine("[W] 12:00:00 | Route /api failed: first line\nsecond line\nthird line"));
                                  });
                              });
                              given("the rows of a hex dump, which carry no level tag", [&] {
                                  then("they are left out", [&] {
                                      check(!isRequestLine("00000010  5b 57 5d 20 20 20 20 20  20 20 20 20 20 20 20 20  |[W]             |"));
                                      check(!isRequestLine("00000000  47 45 54 20 2f 20 48 54  54 50 2f 31 2e 31 0d 0a  |GET / HTTP/1.1..|"));
                                  });
                              });
                          }};
