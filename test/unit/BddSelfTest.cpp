// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-14
//
// WebFront — tests/unit
// Role: Tests of Bdd.hpp: section order, the outcome of each kind of failure, filters, listeners, approx, checkEqual and the report.
// Thread: worker-safe | Alloc: unrestricted
// LLM: some scenarios fail on purpose, main() compares what each did with what is expected

// Tests Bdd.hpp: the order of the sections, the outcome of each kind of failure, the filters and the exit codes, the
// listeners, approx() and the operands checkEqual() reports.
// Some scenarios fail on purpose: main() runs each one and compares what it did with what is expected.

#include <webfront/testing/Bdd.hpp>

#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
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
                                   requireEventually([&] {
                                       return value == 2;
                                   });
                                   checkEventually(
                                       [&] {
                                           return value == 2;
                                       },
                                       1s);
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
                                      check(!checkEventually(
                                          [] {
                                              return false;
                                          },
                                          20ms));
                                      trace.push_back("after check");
                                      requireEventually(
                                          [] {
                                              return false;
                                          },
                                          20ms);
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
                                check(!waitReady(deferred, 5s));   // returns at once too
                                std::future<int> empty;
                                check(!waitReady(empty, 5s));
                                check(!checkReady(empty, 5s));
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

enum class Color { red, blue };
struct Opaque {
    int  value;
    bool operator==(const Opaque&) const = default;
};

}  // namespace

template <>
struct webfront::bdd::Printer<Color> {
    static void append(std::string& text, Color color) {
        text += color == Color::red ? "red" : "blue";
    }
};

namespace {

// approx() refuses the numbers a double cannot always hold: beyond 2^53 a different integer would compare equal.
template <typename T>
constexpr bool approximable = requires(T value) { value == approx(1.0); };
static_assert(approximable<int> && approximable<unsigned> && approximable<float> && approximable<double>);
static_assert(!approximable<std::int64_t> && !approximable<std::uint64_t> && !approximable<bool>);
static_assert(approximable<long double> == (std::numeric_limits<long double>::digits <= std::numeric_limits<double>::digits));
// The same holds for the expected value, unless it is converted explicitly.
template <typename T>
constexpr bool expectable = requires(T value) { approx(value); } && std::is_constructible_v<Approx, T>;
static_assert(expectable<int> && expectable<unsigned> && expectable<float> && expectable<double>);
static_assert(!expectable<std::int64_t> && !expectable<std::uint64_t> && !expectable<bool>);
static_assert(expectable<long double> == (std::numeric_limits<long double>::digits <= std::numeric_limits<double>::digits));

const Scenario approxHolds{"approx holds", [] {
                               require(5.0 + 1e-6 == approx(5.0));
                               require(approx(5.0) == 5.0 + 1e-6);
                               require(5.1 != approx(5.0));
                               require(5.0 + 1e-7 == approx(5.0).margin(1e-6).epsilon(0));
                               require(5.0 + 2e-6 != approx(5.0).margin(1e-6).epsilon(0));
                               require(5.01 == approx(5.0).epsilon(0.01));
                               require(0.0 == approx(1e-9).margin(1e-8));
                               // The difference is compared with the margin, not the rounded bounds expected +/- margin.
                               require(0x1.0000000000001p0 != approx(1.0).epsilon(0).margin(0x1.8p-53));
                               require(0x1.0000000000001p0 == approx(1.0).epsilon(0).margin(0x1p-52));
                               require(3 == approx(3.0));
                               require(std::numeric_limits<double>::infinity() == approx(std::numeric_limits<double>::infinity()));
                               require(!(std::numeric_limits<double>::quiet_NaN() == approx(std::numeric_limits<double>::quiet_NaN())));
                               // An overflow is not taken for the largest finite value, in either direction.
                               constexpr double largest = std::numeric_limits<double>::max(), infinite = std::numeric_limits<double>::infinity();
                               require(infinite != approx(largest));
                               require(-infinite != approx(-largest));
                               require(largest != approx(infinite));
                               require(infinite != approx(largest).margin(infinite));
                               require(infinite != approx(-infinite));
                               require(largest == approx(largest));
                               require(largest != approx(-largest));
                               // A difference of 2 x largest overflows, and so may the relative tolerance: compared halved.
                               require(-largest != approx(largest).epsilon(1.1));
                               require(-largest == approx(largest).epsilon(2.1));
                               require(-largest == approx(largest).epsilon(5.0));
                               require(-largest != approx(largest).margin(largest));
                               require(Approx::defaultEpsilon > 1.1e-5 && Approx::defaultEpsilon < 1.2e-5);
                               requireEqual(5.0 + 1e-6, approx(5.0));
                               requireThrows<std::invalid_argument>([] {
                                   (void)approx(1.0).margin(-1.0);
                               });
                               requireThrows<std::invalid_argument>([] {
                                   (void)approx(1.0).epsilon(std::numeric_limits<double>::quiet_NaN());
                               });
                               trace.push_back("approx");
                           }};

const Scenario approxFails{"approx reports the tolerance", [] {
                               check(5.5 == approx(5.0).margin(0.25));
                               trace.push_back("after check");
                           }};

const Scenario equalHolds{"checkEqual holds", [] {
                              std::vector<int> items{1, 2, 3};
                              requireEqual(items.size(), 3);
                              requireEqual(std::string{"abc"}, "abc");
                              requireEqual(Color::blue, Color::blue);
                              requireEqual(Opaque{1}, Opaque{1});
                              requireEqual(items, std::vector<int>{1, 2, 3});
                              require(!webfront::bdd::impl::equal(-1, 4294967295u));  // compared by value, not converted
                              char text[] = "abc";
                              requireEqual(static_cast<const char*>(text), "abc");    // by characters, not as pointers
                              const char* none = nullptr;
                              require(!webfront::bdd::impl::equal(none, "x"));
                              require(!webfront::bdd::impl::equal(std::string{"x"}, none));
                              requireEqual(none, static_cast<const char*>(nullptr));
                              trace.push_back("equal");
                          }};

// A condition whose explanation cannot be built, and a matcher whose description only converts to std::string.
struct Unexplained {
    explicit operator bool() const {
        return false;
    }
    std::string explain() const {
        throw std::runtime_error{"no explanation"};
    }
};
struct Description {
    operator std::string() const {
        return "is abc";
    }
};
struct IsAbc {
    bool operator()(std::string_view text) const {
        return text == "abc";
    }
    Description describe() const {
        return {};
    }
};

const Scenario explainFails{"a failing explanation", [] {
                                if (!check(Unexplained{}))
                                    trace.push_back("after check");
                                require("abc", IsAbc{});
                                check("abd", !IsAbc{});
                                trace.push_back("matched");
                            }};

const Scenario equalFails{"checkEqual reports its operands", [] {
                              checkEqual(std::vector<int>{1, 2, 3}, std::vector<int>{1, 2, 4});
                              checkEqual(Color::red, Color::blue);
                              checkEqual(Opaque{1}, Opaque{2});
                              checkEqual(std::string{"left text"}, "right text");
                              checkEqual(static_cast<const char*>(nullptr), "x");
                              checkEqual(1.0L, std::nextafter(1.0L, 2.0L));  // written with the precision of a long double
                              trace.push_back("after check");
                              requireEqual(2, 3);
                              trace.push_back("not reached");
                          }};

int runsStarted = 0, runsEnded = 0;
// The size of the trace when the run ended, -1 before any run, and whether the next run-ending listener throws.
long tracedAtEnd     = -1;
bool throwAtEnd      = false;
bool throwAtStart    = false;  // whether the scenario-starting listener throws
bool throwAtRunStart = false;  // whether the run-starting listener throws
bool failAtEnd       = false;  // whether the run-ending listener records a failure with check()

const Listener runListener{[] {
                               ++runsStarted;
                               if (throwAtRunStart)
                                   throw std::runtime_error{"cannot reach the device"};
                           },
                           [] {
                               if (throwAtStart)
                                   throw std::runtime_error{"cannot open the recording"};
                           },
                           [] {
                               ++runsEnded;
                               tracedAtEnd = static_cast<long>(trace.size());
                               if (throwAtEnd)
                                   throw std::runtime_error{"cannot save the recording"};
                               check(!failAtEnd);
                           }};

std::string readFile(const std::string& name) {
    std::ifstream file{name};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

int failures = 0;

void expect(std::string_view title, int code, std::vector<std::string> expectedTrace) {
    trace.clear();
    std::string        exact{title};
    std::vector<char*> arguments{const_cast<char*>("bdd_self_test"), const_cast<char*>("--exact"), exact.data()};
    auto               started = runsStarted, ended = runsEnded;
    auto               result = run(static_cast<int>(arguments.size()), arguments.data());
    // Each run of a selected scenario notifies its start and its end once.
    if (runsStarted != started + 1 || runsEnded != ended + 1) {
        ++failures;
        std::cerr << "MISMATCH in \"" << title << "\": " << runsStarted - started << " run starts, " << runsEnded - ended << " run endings\n";
    }
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
    expect("approx holds", 0, {"approx"});
    expect("approx reports the tolerance", 1, {"after check"});
    expect("checkEqual holds", 0, {"equal"});
    expect("a failing explanation", 1, {"after check", "matched"});
    expect("checkEqual reports its operands", 1, {"after check"});

    // A run-ending listener runs once per run that started, after the last scenario; one that throws fails the run.
    if (runsStarted == 0 || runsEnded != runsStarted || tracedAtEnd != 1) {
        ++failures;
        std::cerr << "MISMATCH: " << runsStarted << " runs started, " << runsEnded << " ended, " << tracedAtEnd << " steps traced at the end\n";
    }
    throwAtEnd = true;
    expect("approx holds", 1, {"approx"});
    // The JUnit report shows that error and its message, beside the scenario that passed.
    {
        std::string report = "bdd_self_test_report.xml";
        expectExit({"--exact", "approx holds", "--junit", report}, 1);
        auto xml = readFile(report);
        for (std::string_view part : {"tests=\"2\" failures=\"0\" errors=\"1\"",
                                      "<testcase classname=\"tests\" name=\"Scenario: approx holds\"",
                                      "<testcase classname=\"tests\" name=\"Run ending\" time=\"0.000\">\n      <error "
                                      "message=\"a run-ending listener failed\">cannot save the recording\n</error>"})
            if (xml.find(part) == std::string::npos) {
                ++failures;
                std::cerr << "MISMATCH: the report of a failed run ending lacks \"" << part << "\"\n";
            }
        std::remove(report.c_str());
    }
    throwAtEnd = false;
    // A failure the run-ending listener records with check(), without throwing, also fails the run and is reported;
    // the failure of a condition whose explain() throws is recorded with the message of that exception.
    {
        failAtEnd          = true;
        std::string report = "bdd_self_test_report.xml";
        expectExit({"--exact", "approx holds", "--junit", report}, 1);
        failAtEnd = false;
        auto xml  = readFile(report);
        if (xml.find("tests=\"2\" failures=\"0\" errors=\"1\"") == std::string::npos
            || xml.find("message=\"a run-ending listener failed\">") == std::string::npos || xml.find("in Scenario: Run ending") == std::string::npos) {
            ++failures;
            std::cerr << "MISMATCH: the report of a failure recorded by a run-ending listener\n" << xml;
        }
        expectExit({"--exact", "a failing explanation", "--junit", report}, 1);
        if (readFile(report).find("(explain() threw: no explanation)") == std::string::npos) {
            ++failures;
            std::cerr << "MISMATCH: the report of a failing explanation\n";
        }
        std::remove(report.c_str());
    }
    // A scenario-starting listener that throws fails its scenario, which does not run, and the run still ends.
    {
        throwAtStart       = true;
        auto        before = runsEnded;
        std::string report = "bdd_self_test_report.xml";
        trace.clear();
        expectExit({"--exact", "approx holds", "--junit", report}, 1);
        throwAtStart = false;
        auto xml     = readFile(report);
        if (!trace.empty() || runsEnded != before + 1 || xml.find("failures=\"1\"") == std::string::npos
            || xml.find("a scenario-starting listener threw: cannot open the recording") == std::string::npos) {
            ++failures;
            std::cerr << "MISMATCH: a throwing scenario-starting listener: " << trace.size() << " steps, " << runsEnded - before << " run endings, report\n"
                      << xml;
        }
        std::remove(report.c_str());
    }
    // A run-starting listener that throws: no scenario runs, the run-ending listeners are still called, and the JUnit
    // report shows that error.
    {
        throwAtRunStart    = true;
        auto        before = runsEnded;
        std::string report = "bdd_self_test_report.xml";
        trace.clear();
        expectExit({"--exact", "approx holds", "--junit", report}, 1);
        throwAtRunStart = false;
        auto xml        = readFile(report);
        if (!trace.empty() || runsEnded != before + 1 || xml.find("tests=\"1\" failures=\"0\" errors=\"1\"") == std::string::npos
            || xml.find("<testcase classname=\"tests\" name=\"Run starting\" time=\"0.000\">\n      <error "
                        "message=\"a run-starting listener failed\">cannot reach the device\n</error>")
                   == std::string::npos) {
            ++failures;
            std::cerr << "MISMATCH: a throwing run-starting listener: " << trace.size() << " steps, " << runsEnded - before << " run endings, report\n" << xml;
        }
        std::remove(report.c_str());
    }
    auto unchanged = runsEnded;
    expectExit({"[unknown]"}, 2);
    if (runsEnded != unchanged) {
        ++failures;
        std::cerr << "MISMATCH: a run that selects nothing called the run-ending listener\n";
    }

    // A failure names the expected value, the tolerances and the difference, and the operands of checkEqual.
    {
        std::string report = "bdd_self_test_report.xml";
        expectExit({"--exact", "approx reports the tolerance", "--junit", report}, 1);
        auto xml = readFile(report);
        for (std::string_view part : {"5.5 == approx(5) (margin 0.25, epsilon 1.19209e-05): the difference is 0.5"})
            if (xml.find(part) == std::string::npos) {
                ++failures;
                std::cerr << "MISMATCH: the report of approx lacks \"" << part << "\"\n";
            }
        expectExit({"--exact", "checkEqual reports its operands", "--junit", report}, 1);
        xml = readFile(report);
        for (std::string_view part : {"left:  {1, 2, 3}\n  right: {1, 2, 4}",
                                      "left:  red\n  right: blue",
                                      "left:  {?}\n  right: {?}",
                                      "left:  left text\n  right: right text",
                                      "left:  nullptr\n  right: x",
                                      "left:  1\n  right: 1.0000000000000",
                                      "left:  2\n  right: 3"})
            if (xml.find(part) == std::string::npos) {
                ++failures;
                std::cerr << "MISMATCH: the report of checkEqual lacks \"" << part << "\"\n";
            }
        std::remove(report.c_str());
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
            if (webfront::bdd::impl::parseFactor("2.5") != 2.5) {
                ++failures;
                std::cerr << "MISMATCH: the timeout factor depends on the locale " << locale << '\n';
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
