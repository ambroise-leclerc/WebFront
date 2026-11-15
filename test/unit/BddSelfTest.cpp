// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Tests Bdd.hpp: the order of the sections, the outcome of each kind of failure, the filters and the exit codes.
/// Some scenarios fail on purpose: main() runs each one and compares what it did with what is expected.
#include "Bdd.hpp"

#include <chrono>
#include <clocale>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace webfront::bdd;

namespace {

std::vector<std::string> trace;

const Scenario sectionsScenario{"sections", [] {
                                    trace.push_back("start");
                                    given("a", [&] {
                                        trace.push_back("a");
                                        when("a1", [&] {
                                            trace.push_back("a1");
                                            then("x", [&] {
                                                trace.push_back("x");
                                            });
                                            then("y", [&] {
                                                trace.push_back("y");
                                            });
                                        });
                                        when("a2", [&] {
                                            trace.push_back("a2");
                                        });
                                    });
                                    given("b", [&] {
                                        trace.push_back("b");
                                    });
                                }};

const Scenario sameNames{"sections of the same name", [] {
                             given("data", [&] {
                                 trace.push_back("first");
                             });
                             given("data", [&] {
                                 trace.push_back("second");
                             });
                             for (int value : {1, 2})
                                 when("value ", value, [&] {
                                     trace.push_back("value " + std::to_string(value));
                                 });
                         }};

// Sections of the same name and the same body, as an identical code folding linker would merge, stay two sections.
const Scenario sameComposed{"composed sections of the same name", [] {
                                given("same ", 1, [&] {
                                    trace.push_back("x");
                                });
                                given("same ", 1, [&] {
                                    trace.push_back("x");
                                });
                            }};

const Scenario checkGoesOn{"check goes on", [] {
                               given("a failed check", [&] {
                                   check(false);
                                   trace.push_back("after check");
                               });
                               given("another section", [&] {
                                   trace.push_back("other");
                               });
                           }};

const Scenario requireStops{"require stops", [] {
                                given("a failed require", [&] {
                                    try {
                                        require(1 + 1 == 3);
                                    } catch (const std::exception&) {
                                        trace.push_back("swallowed");
                                    }
                                    trace.push_back("after require");
                                });
                                given("another section", [&] {
                                    trace.push_back("other");
                                });
                            }};

const Scenario nestedFailure{"a nested failure", [] {
                                 given("a", [&] {
                                     trace.push_back("a");
                                     when("a1", [&] {
                                         then("fails", [&] {
                                             require(false);
                                         });
                                     });
                                     when("a2", [&] {
                                         trace.push_back("a2");
                                     });
                                     require(true);
                                 });
                             }};

const Scenario failureAfterSections{"a failure after the sections", [] {
                                        given("a", [&] {
                                            when("a1", [&] {
                                                trace.push_back("a1");
                                            });
                                            when("a2", [&] {
                                                trace.push_back("a2");
                                            });
                                            fail("after the sections");
                                        });
                                    }};

const Scenario exceptionReported{"an exception is a failure", [] {
                                     when("a section throws", [&] {
                                         throw std::runtime_error{"boom"};
                                     });
                                     when("another section", [&] {
                                         trace.push_back("other");
                                     });
                                 }};

const Scenario skippedScenario{"skip", {"skipping"}, [] {
                                   skip("not here");
                               }};

const Scenario assertionsPass{
    "assertions pass",
    {"passing", "tagged"},
    [] {
        std::string text{"Hello, world"};
        require(text, contains("lo, w"));
        require(text, endsWith("world"));
        require(text, equals("Hello, world"));
        check(!text.empty());
        requireThrows([] {
            throw std::logic_error{"bad"};
        });
        requireThrows<std::logic_error>([] {
            throw std::logic_error{"bad"};
        });
        requireThrows<std::logic_error>(
            [] {
                throw std::logic_error{"bad thing"};
            },
            contains("thing"));
        requireThrows<int>([] {
            throw 3;
        });
        requireNothrow([] { });
        std::optional<int> value{1};
        require(value);
        auto scope = info("value ", *value);
        succeed("done");
      }
};

const Scenario controlCharacters{"a failure with a control character", [] {
                                     auto scope = info(std::string("a\0b", 3));
                                     check(false);
                                 }};

const Scenario invalidText{"a failure with invalid UTF-8", [] {
                               auto scope = info("caf\xC3\xA9 ", std::string("\xC3"), " \xED\xA0\x80");
                               check(false);
                           }};

const Scenario concurrentFailures{"failures on several threads", [] {
                                      std::vector<std::thread> threads;
                                      for (int index = 0; index != 4; ++index)
                                          threads.emplace_back([] {
                                              for (int repeat = 0; repeat != 50; ++repeat)
                                                  check(false);
                                          });
                                      for (auto& thread : threads)
                                          thread.join();
                                  }};

const Scenario throwsMismatch{"throws of another type", [] {
                                  requireThrows<std::invalid_argument>([] {
                                      throw std::logic_error{"bad"};
                                  });
                              }};

const Scenario eventuallyHolds{"eventually holds", [] {
                                   using namespace std::chrono_literals;
                                   std::atomic<int> value{0};
                                   std::thread      worker{[&] {
                                       std::this_thread::sleep_for(20ms);
                                       value = 2;
                                   }};
                                   requireEventually([&] { return value == 2; });
                                   check(checkEventually([&] { return value == 2; }, 1s));
                                   worker.join();
                                   std::promise<int> promise;
                                   auto              future = promise.get_future();
                                   promise.set_value(1);
                                   requireReady(future);
                                   check(checkReady(future, 1s));
                                   check(future.get() == 1);
                                   trace.push_back("held");
                               }};

const Scenario eventuallyTimesOut{"eventually times out", [] {
                                      using namespace std::chrono_literals;
                                      check(!checkEventually([] { return false; }, 20ms));
                                      trace.push_back("after check");
                                      requireEventually([] { return false; }, 20ms);
                                      trace.push_back("not reached");
                                  }};

const Scenario readyTimesOut{"a future that is never ready", [] {
                                 using namespace std::chrono_literals;
                                 std::promise<void> promise;
                                 auto               future = promise.get_future();
                                 check(!checkReady(future, 10ms));
                                 trace.push_back("after check");
                                 requireReady(future, 10ms);
                                 trace.push_back("not reached");
                             }};

bool alwaysTrue() {
    return true;
}

const Scenario waitingForms{"waiting forms", [] {
                                using namespace std::chrono_literals;
                                requireEventually(alwaysTrue);
                                requireEventually(&alwaysTrue, 1s);
                                check(waitUntil(alwaysTrue));
                                check(!waitUntil(
                                    [] {
                                        return false;
                                    },
                                    5ms));  // records nothing
                                std::future<int> deferred = std::async(std::launch::deferred, [] {
                                    return 1;
                                });
                                check(!waitReady(deferred, 5ms));
                                check(!checkReady(deferred, 5s));  // fails at once, not after 5 s
                                trace.push_back("deferred");
                            }};

const Scenario timeoutReport{"a timeout in a section", [] {
                                 using namespace std::chrono_literals;
                                 given("a waiting test", [&] {
                                     const auto context = info("waiting for call ", 3);
                                     checkEventually(
                                         [] {
                                             return false;
                                         },
                                         5ms);
                                 });
                             }};

int failures = 0;

void expect(std::string_view title, int code, std::vector<std::string> expectedTrace) {
    trace.clear();
    std::string        exact{title};
    std::vector<char*> arguments{const_cast<char*>("bdd_self_test"), const_cast<char*>("--exact"), exact.data()};
    auto               result = run(static_cast<int>(arguments.size()), arguments.data());
    if (result != code || trace != expectedTrace) {
        ++failures;
        std::cerr << "MISMATCH in \"" << title << "\": exit " << result << " (expected " << code << "), trace";
        for (const auto& step : trace)
            std::cerr << ' ' << step;
        std::cerr << '\n';
    }
}

void expectExit(std::vector<std::string> options, int code) {
    std::vector<char*> arguments{const_cast<char*>("bdd_self_test")};
    for (auto& option : options)
        arguments.push_back(option.data());
    if (auto result = run(static_cast<int>(arguments.size()), arguments.data()); result != code) {
        ++failures;
        std::cerr << "MISMATCH for the options";
        for (const auto& option : options)
            std::cerr << ' ' << option;
        std::cerr << ": exit " << result << " (expected " << code << ")\n";
    }
}

}  // namespace

int main() {
    expect("sections", 0, {"start", "a", "a1", "x", "start", "a", "a1", "y", "start", "a", "a2", "start", "b"});
    expect("sections of the same name", 0, {"first", "second", "value 1", "value 2"});
    expect("composed sections of the same name", 0, {"x", "x"});
    expect("check goes on", 1, {"after check", "other"});
    expect("require stops", 1, {"other"});
    expect("an exception is a failure", 1, {"other"});
    expect("a nested failure", 1, {"a", "a", "a", "a2"});
    expect("a failure after the sections", 1, {"a1"});
    expect("skip", 4, {});
    expect("assertions pass", 0, {});
    expect("throws of another type", 1, {});
    expect("eventually holds", 0, {"held"});
    expect("waiting forms", 1, {"deferred"});
    expect("eventually times out", 1, {"after check"});
    expect("a future that is never ready", 1, {"after check"});
    {
        using webfront::bdd::impl::parseFactor;
        using webfront::bdd::impl::timeoutMessage;
        for (auto [factor, expected] : {
                 std::pair{ 1.0,                     std::string{"not met within 30.000000 s (waited 30.500000 s)"}},
                 std::pair{10.0, std::string{"not met within 300.000000 s (30.000000 s x 10) (waited 30.500000 s)"}}
        })
            if (auto text = timeoutMessage(std::chrono::milliseconds{30000}, factor, 30.5); text != expected) {
                ++failures;
                std::cerr << "MISMATCH: the report of a timeout is \"" << text << "\"\n";
            }
        for (auto [text, expected] : {
                 std::pair<const char*, double>{nullptr, 1.0},
                 {     "", 1.0},
                 {    "x", 1.0},
                 {   "-2", 1.0},
                 {    "0", 1.0},
                 {   "3x", 1.0},
                 {  "1e2", 1.0},
                 {   ".5", 1.0},
                 {   "2.", 1.0},
                 {  "inf", 1.0},
                 { "0x10", 1.0},
                 {    "3", 3.0},
                 {  "0.5", 0.5},
                 {  "2.5", 2.5}
        })
            if (parseFactor(text) != expected) {
                ++failures;
                std::cerr << "MISMATCH: parseFactor(" << (text ? text : "null") << ")\n";
            }
    }
    expectExit({"[passing]"}, 0);
    expectExit({"[passing][tagged]"}, 0);
    expectExit({"[skipping],[passing]"}, 0);
    expectExit({"Scenario: assertions pass"}, 0);
    expectExit({"assertions*", "--order", "rand", "--rng-seed", "7"}, 0);
    expectExit({"[unknown]"}, 2);
    expectExit({"--exact", "failures on several threads"}, 1);

    // XML 1.0 forbids the control characters a message may hold: the JUnit report writes them as \xNN.
    std::string report = "bdd_self_test_report.xml";
    expectExit({"--exact", "a failure with a control character", "--junit", report}, 1);
    std::ifstream     file{report};
    const std::string xml{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (xml.find('\0') != std::string::npos || xml.find("a\\x00b") == std::string::npos) {
        ++failures;
        std::cerr << "MISMATCH: the JUnit report does not escape a NUL\n";
    }
    // Durations are decimal numbers with six digits after the point, never in scientific notation.
    for (auto position = xml.find("time=\""); position != std::string::npos; position = xml.find("time=\"", position + 1)) {
        auto start = position + 6, end = xml.find('"', start);
        auto value = xml.substr(start, end - start);
        auto point = value.find('.');
        if (point == std::string::npos || point == 0 || value.size() - point != 7 || value.find_first_not_of("0123456789.") != std::string::npos) {
            ++failures;
            std::cerr << "MISMATCH: the JUnit duration " << value << " is not a fixed-point number\n";
        }
    }
    file.close();
    std::remove(report.c_str());

    // The report of a timeout names the delay and the time waited, the section and the info messages.
    expectExit({"--exact", "a timeout in a section", "--junit", report}, 1);
    file.open(report);
    const std::string timeoutXml{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    for (std::string_view part : {"not met within ", "Given: a waiting test", "with: waiting for call 3", "BddSelfTest.cpp:"})
        if (timeoutXml.find(part) == std::string::npos) {
            ++failures;
            std::cerr << "MISMATCH: the report of a timeout lacks \"" << part << "\"\n";
        }
    file.close();
    std::remove(report.c_str());

    // Bytes that are not UTF-8 (a truncated sequence, an encoded surrogate) are escaped; valid UTF-8 is kept.
    expectExit({"--exact", "a failure with invalid UTF-8", "--junit", report}, 1);
    file.open(report);
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (text.find("caf\xC3\xA9 \\xc3 \\xed\\xa0\\x80") == std::string::npos) {
        ++failures;
        std::cerr << "MISMATCH: the JUnit report does not escape invalid UTF-8\n";
    }
    file.close();
    std::remove(report.c_str());

    // CTest names each scenario from one line of --list, in a bracket argument.
    if (webfront::bdd::impl::titleError("a\nb").empty() || webfront::bdd::impl::titleError("a]==]b").empty() || webfront::bdd::impl::titleError("").empty()
        || !webfront::bdd::impl::titleError("a [b] c").empty()) {
        ++failures;
        std::cerr << "MISMATCH: titleError\n";
    }

    // A number keeps its decimal point whatever the locale.
    for (const char* locale : {"fr_FR.UTF-8", "fr_FR.utf8", "de_DE.UTF-8"})
        if (std::setlocale(LC_NUMERIC, locale)) {
            if (webfront::bdd::impl::concat(0.5) != "0.5") {
                ++failures;
                std::cerr << "MISMATCH: the decimal point depends on the locale " << locale << '\n';
            }
            std::setlocale(LC_NUMERIC, "C");
            break;
        }
    expectExit({"--shard-count", "2", "--shard-index", "2"}, 2);
    expectExit({"--unknown"}, 2);
    expectExit({"[passing]", "--list"}, 0);
    std::cout << (failures ? "Bdd self-test FAILED\n" : "Bdd self-test passed\n");
    return failures ? 1 : 0;
}
