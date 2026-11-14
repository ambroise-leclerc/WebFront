// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Tests Bdd.hpp: the order of the sections, the outcome of each kind of failure, the filters and the exit codes.
/// Some scenarios fail on purpose: main() runs each one and compares what it did with what is expected.
#include "Bdd.hpp"

#include <clocale>
#include <cstdio>
#include <fstream>
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
