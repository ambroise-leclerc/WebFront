// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-26
//
// WebFront — tests/unit
// Role: What an application registered and runs, as the inspectors read it: functions, their counters, JavaScript calls, browsers.
// Thread: bridge-io | Alloc: unrestricted
// LLM: tests of ADR-027 section 7 with the injectable networking mock; the access rules are in ViewAssociationTests.cpp

#include "InjectableNetworking.hpp"
#include <webfront/Introspection.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/Space.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

using namespace webfront::bdd;
using namespace webfront;

namespace {

using IntroSocket = testing::InjectableSocket;

const std::string introUpgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

struct IntroNet : testing::InjectableNetworking {
    static inline bool accepted{};
    static auto        Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (std::exchange(accepted, true))
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

using IntroApp = BasicWF<IntroNet, fs::DefaultFS>;

std::vector<testing::ManualSender::Completion> introGates;

testing::ManualSender introGate() {
    return testing::ManualSender([](testing::ManualSender::Completion completion) {
        introGates.push_back(std::move(completion));
    });
}

ResultTask<std::string> introLater(std::string value) {
    co_await introGate();
    co_return "later:" + value;
}

int introTwice(int value) {
    return 2 * value;
}

/// The application with one browser, which calls its functions.
struct IntroFresh {
    IntroFresh() {
        IntroSocket::reset();
        introGates.clear();
        IntroNet::accepted = false;
    }
};

struct IntroBrowser {
    ~IntroBrowser() {
        app.shutdown();
        IntroSocket::reset();
        introGates.clear();
    }
    void connect() {
        IntroSocket::receive(std::as_bytes(std::span(introUpgradeRequest)));
        msg::Handshake handshake;
        IntroSocket::receive(testing::clientFrame(std::span(reinterpret_cast<const std::byte*>(handshake.header().data()), handshake.header().size())));
    }
    template <typename... Arguments>
    void call(const std::string& name, msg::CallId callId, const Arguments&... arguments) {
        msg::FunctionCall<> message;
        message.setCallId(callId);
        websocket::Frame<IntroNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
        message.encodeParameter(name, frame);
        (message.encodeParameter(arguments, frame), ...);
        frame.freeze();
        IntroSocket::receive(testing::clientFrame(testing::messagePayload(frame)));
    }
    [[nodiscard]] inspect::FunctionInfo function(std::string_view name) const {
        for (auto& info : app.registeredFunctions())
            if (info.name == name)
                return info;
        throw std::logic_error("No function named " + std::string(name));
    }

    IntroFresh fresh;  // resets the shared mock socket before the application accepts its connection
    IntroApp   app{WebFrontConfig{.port = "0"}};
};

}  // namespace

const Scenario introTypescriptTypes{"The types of the bridge are written as TypeScript declares them (ADR-027, section 7)", {"inspector"}, [] {
                                        then("scalars and strings have their TypeScript names", [] {
                                            requireEqual(inspect::typescriptType<void>(), std::string{"void"});
                                            requireEqual(inspect::typescriptType<bool>(), std::string{"boolean"});
                                            requireEqual(inspect::typescriptType<int>(), std::string{"number"});
                                            requireEqual(inspect::typescriptType<double>(), std::string{"number"});
                                            requireEqual(inspect::typescriptType<std::size_t>(), std::string{"number"});
                                            requireEqual(inspect::typescriptType<const std::string&>(), std::string{"string"});
                                            requireEqual(inspect::typescriptType<std::string_view>(), std::string{"string"});
                                        });
                                        then("numbers in a container are a typed array, and a bitset its packed bytes", [] {
                                            requireEqual(inspect::typescriptType<std::vector<std::uint8_t>>(), std::string{"Uint8Array"});
                                            requireEqual(inspect::typescriptType<std::vector<std::int32_t>>(), std::string{"Int32Array"});
                                            requireEqual(inspect::typescriptType<std::array<float, 3>>(), std::string{"Float32Array"});
                                            requireEqual(inspect::typescriptType<std::span<const double>>(), std::string{"Float64Array"});
                                            requireEqual(inspect::typescriptType<std::vector<std::uint64_t>>(), std::string{"BigUint64Array"});
                                            requireEqual(inspect::typescriptType<std::bitset<9>>(), std::string{"Uint8Array"});
                                        });
                                        then("tuples, pairs and lists are written from their elements", [] {
                                            requireEqual(inspect::typescriptType<std::tuple<int, std::string>>(), std::string{"[number, string]"});
                                            requireEqual(inspect::typescriptType<std::pair<bool, double>>(), std::string{"[boolean, number]"});
                                            requireEqual(inspect::typescriptType<std::vector<std::string>>(), std::string{"string[]"});
                                            requireEqual(inspect::typescriptType<std::vector<std::tuple<double, std::string>>>(),
                                                         std::string{"[number, string][]"});
                                            requireEqual(inspect::typescriptType<std::tuple<std::vector<std::string>, double>>(),
                                                         std::string{"[string[], number]"});
                                        });
                                        then("a type the bridge does not carry is unknown", [] {
                                            struct Opaque { };
                                            requireEqual(inspect::typescriptType<Opaque>(), std::string{"unknown"});
                                        });
                                        then("a signature names its parameters and always returns a Promise", [] {
                                            requireEqual(inspect::typescriptSignature({}, "void"), std::string{"() => Promise<void>"});
                                            requireEqual(inspect::typescriptSignature({"number", "string"}, "boolean"),
                                                         std::string{"(a: number, b: string) => Promise<boolean>"});
                                            const auto many = inspect::typescriptSignature(std::vector<std::string>(27, "number"), "void");
                                            require(many.contains("z: number, a1: number"));
                                            const auto bounded = inspect::typescriptSignature(std::vector<std::string>(200, "Float64Array"), "void");
                                            require(bounded.size() <= inspect::maxSignatureBytes);
                                            require(bounded.ends_with("\xE2\x80\xA6"));
                                        });
                                    }};

const Scenario introMetadata{"Each registered function keeps its name, its TypeScript signature and where it was registered (ADR-027, section 7)",
                             {"inspector"},
                             [] {
                                 IntroBrowser browser;
                                 auto&        app = browser.app;

                                 given("a lambda", [&] {
                                     const auto line = std::source_location::current().line() + 1;
                                     app.cppFunction("add", [](int a, double b) {
                                         return a + b;
                                     });
                                     const auto info = browser.function("add");
                                     then("its signature is deduced", [&] {
                                         requireEqual(info.signature, std::string{"(a: number, b: number) => Promise<number>"});
                                         require(!info.takesCaller && !info.deferred);
                                     });
                                     then("the file and the line of the registration are kept", [&] {
                                         require(info.location.ends_with("IntrospectionTests.cpp:" + std::to_string(line)));
                                     });
                                 });
                                 given("a function pointer", [&] {
                                     app.cppFunction("twice", &introTwice);
                                     requireEqual(browser.function("twice").signature, std::string{"(a: number) => Promise<number>"});
                                 });
                                 given("a std::function", [&] {
                                     app.cppFunction("greet", std::function<std::string(const std::string&, bool)>{[](const std::string& name, bool) {
                                                         return name;
                                                     }});
                                     requireEqual(browser.function("greet").signature, std::string{"(a: string, b: boolean) => Promise<string>"});
                                 });
                                 given("a function without result", [&] {
                                     app.cppFunction("notify", [](const std::vector<std::string>&, std::tuple<int, std::string>) { });
                                     requireEqual(browser.function("notify").signature, std::string{"(a: string[], b: [number, string]) => Promise<void>"});
                                 });
                                 given("a function that takes the calling UI", [&] {
                                     app.cppFunction("whoami", [](IntroApp::UI ui, std::string) {
                                         return static_cast<int>(ui.id());
                                     });
                                     const auto info = browser.function("whoami");
                                     then("the caller is told, and is not a parameter of JavaScript", [&] {
                                         require(info.takesCaller);
                                         requireEqual(info.signature, std::string{"(a: string) => Promise<number>"});
                                     });
                                 });
                                 given("a function that answers later", [&] {
                                     app.cppFunction("later", &introLater);
                                     const auto info = browser.function("later");
                                     then("its result is that of its task", [&] {
                                         require(info.deferred && !info.takesCaller);
                                         requireEqual(info.signature, std::string{"(a: string) => Promise<string>"});
                                     });
                                 });
                                 given("the explicit form, for a generic lambda", [&] {
                                     const auto line = std::source_location::current().line() + 1;
                                     app.cppFunction<std::string, std::vector<std::uint8_t>>("hex", [](const auto&) {
                                         return std::string{};
                                     });
                                     const auto info = browser.function("hex");
                                     then("the signature is the one given, and the location that of the call", [&] {
                                         requireEqual(info.signature, std::string{"(a: Uint8Array) => Promise<string>"});
                                         require(info.location.ends_with("IntrospectionTests.cpp:" + std::to_string(line)));
                                     });
                                 });
                                 given("a name that is taken", [&] {
                                     app.cppFunction("once", [] {
                                         return 1;
                                     });
                                     require(!app.cppFunction("once", [](std::string) { }));
                                     then("the function that stays registered is the one listed, once", [&] {
                                         const auto functions = app.registeredFunctions();
                                         requireEqual(std::ranges::count_if(functions,
                                                                            [](const auto& info) {
                                                                                return info.name == "once";
                                                                            }),
                                                      std::ptrdiff_t{1});
                                         requireEqual(browser.function("once").signature, std::string{"() => Promise<number>"});
                                     });
                                 });
                             }};

const Scenario introCounters{"The calls of a function are counted, with its failures and its last error (ADR-027, section 7)", {"inspector"}, [] {
                                 IntroBrowser browser;
                                 auto&        app = browser.app;
                                 app.cppFunction("twice", &introTwice);
                                 app.cppFunction("fails", [](const std::string& reason) -> int {
                                     throw std::runtime_error(reason);
                                 });
                                 app.cppFunction("failsAtLength", []() -> int {
                                     throw std::runtime_error(std::string{"a\nb"} + std::string(1000, 'x'));
                                 });
                                 app.cppFunction("later", &introLater);
                                 browser.connect();

                                 when("a call succeeds", [&] {
                                     browser.call("twice", 3, 21);
                                     browser.call("twice", 4, 4);
                                     const auto info = browser.function("twice");
                                     then("it is counted, without failure", [&] {
                                         requireEqual(info.calls, std::uint64_t{2});
                                         requireEqual(info.failures, std::uint64_t{0});
                                         require(info.lastError.empty());
                                     });
                                     then("no duration was measured: no inspector is reading", [&] {
                                         requireEqual(info.timedCalls, std::uint64_t{0});
                                     });
                                 });
                                 when("the callback throws", [&] {
                                     browser.call("fails", 5, std::string{"the disk is full"});
                                     const auto info = browser.function("fails");
                                     then("the failure and its message are kept", [&] {
                                         requireEqual(info.calls, std::uint64_t{1});
                                         requireEqual(info.failures, std::uint64_t{1});
                                         requireEqual(info.lastError, std::string{"the disk is full"});
                                     });
                                 });
                                 when("the message of the failure is long and holds control characters", [&] {
                                     browser.call("failsAtLength", 5);
                                     const auto info = browser.function("failsAtLength");
                                     then("it is made visible and bounded", [&] {
                                         require(info.lastError.size() <= inspect::maxTextBytes);
                                         require(info.lastError.starts_with("a\xE2\x90\x8A"
                                                                            "b"));
                                         require(info.lastError.ends_with("\xE2\x80\xA6"));
                                     });
                                 });
                                 when("the arguments cannot be decoded as the function declares them", [&] {
                                     browser.call("twice", 6, std::string{"not a number"});
                                     then("the call fails before the callback", [&] {
                                         const auto info = browser.function("twice");
                                         requireEqual(info.calls, std::uint64_t{1});
                                         requireEqual(info.failures, std::uint64_t{1});
                                         require(!info.lastError.empty());
                                     });
                                 });
                                 when("a function that answers later completes", [&] {
                                     browser.call("later", 7, std::string{"x"});
                                     then("the call is counted when it starts", [&] {
                                         requireEqual(browser.function("later").calls, std::uint64_t{1});
                                         requireEqual(browser.function("later").failures, std::uint64_t{0});
                                     });
                                     introGates.at(0).complete({}, 0);
                                     then("its answer is not a failure", [&] {
                                         require(IntroSocket::wrote("later:x"));
                                         requireEqual(browser.function("later").failures, std::uint64_t{0});
                                     });
                                 });
                                 when("the browser disconnects before a function answers", [&] {
                                     browser.call("later", 8, std::string{"x"});
                                     IntroSocket::fail(std::make_error_code(std::errc::connection_reset));
                                     const auto info = browser.function("later");
                                     then("the call that gave no result is a failure, and its error says it was cancelled", [&] {
                                         requireEqual(info.calls, std::uint64_t{1});
                                         requireEqual(info.failures, std::uint64_t{1});
                                         require(info.lastError.contains("cancelled"));
                                     });
                                 });
                             }};

const Scenario introTiming{"The duration of a call is measured only while an inspector reads the functions (ADR-027, section 7)", {"inspector"}, [] {
                               IntroBrowser browser;
                               auto&        app = browser.app;
                               app.cppFunction("twice", &introTwice);
                               browser.connect();

                               given("the gate that opens the measure", [&] {
                                   inspect::TimingGate gate;
                                   then("it is closed until an inspector reads, and a call then reads no clock", [&] {
                                       require(!gate.isOpen());
                                       require(!gate.begin());
                                   });
                                   then("a read opens it for a while, and the first call after that while closes it", [&] {
                                       gate.observe();
                                       require(gate.begin().has_value());
                                       gate.observe(std::chrono::seconds{-1});
                                       require(gate.isOpen());
                                       require(!gate.begin());
                                       require(!gate.isOpen());
                                   });
                               });
                               when("no inspector has read the functions", [&] {
                                   browser.call("twice", 3, 1);
                                   then("the call is counted and not timed", [&] {
                                       requireEqual(browser.function("twice").calls, std::uint64_t{1});
                                       requireEqual(browser.function("twice").timedCalls, std::uint64_t{0});
                                   });
                               });
                               when("an inspector reads the functions, then a call is made", [&] {
                                   inspect::FunctionRegistry registry;
                                   const auto record = registry.add("timed", "() => Promise<void>", false, false, std::source_location::current());
                                   static_cast<void>(registry.page(0, 200, inspect::minPageBytes));
                                   require(registry.timing());
                                   { const inspect::CallMeasure measure{record}; }
                                   { const inspect::CallMeasure measure{record}; }
                                   const auto info = registry.find("timed");
                                   then("each call is timed, and the mean and the longest duration are given in milliseconds", [&] {
                                       require(info.has_value());
                                       requireEqual(info->timedCalls, std::uint64_t{2});
                                       require(info->maxDuration <= info->totalDuration);
                                       const auto row = inspect::FunctionRegistry::row(*info);
                                       require((static_cast<unsigned>(std::get<5>(row)) & inspect::function_flag::timed) != 0);
                                       require(std::get<9>(row) >= 0 && std::get<9>(row) <= std::get<10>(row));
                                   });
                                   andWhen("the inspector stops reading for longer than the window", [&] {
                                       registry.observe(std::chrono::seconds{-1});
                                       { const inspect::CallMeasure measure{record}; }
                                       then("the next call is counted, not timed, and closes the measure", [&] {
                                           requireEqual(registry.find("timed")->calls, std::uint64_t{3});
                                           requireEqual(registry.find("timed")->timedCalls, std::uint64_t{2});
                                           require(!registry.timing());
                                       });
                                   });
                               });
                               when("a call answered later is timed", [&] {
                                   inspect::FunctionRegistry registry;
                                   const auto record = registry.add("later", "() => Promise<void>", false, true, std::source_location::current());
                                   registry.observe();
                                   inspect::CallMeasure started{record};
                                   inspect::CallMeasure carried{std::move(started)};
                                   then("the measure goes with the call, and ends once", [&] {
                                       requireEqual(registry.find("later")->timedCalls, std::uint64_t{0});
                                       carried = {};
                                       requireEqual(registry.find("later")->timedCalls, std::uint64_t{1});
                                       requireEqual(registry.find("later")->calls, std::uint64_t{1});
                                   });
                               });
                           }};

const Scenario introFunctionPages{"The functions are read page after page, each within the budget of an answer (ADR-027, sections 3 and 7)", {"inspector"}, [] {
                                      inspect::FunctionRegistry                             registry;
                                      std::vector<std::shared_ptr<inspect::FunctionRecord>> owned;
                                      const std::string                                     longName(1000, 'n');
                                      for (int i = 0; i < 5; ++i)
                                          owned.push_back(registry.add(longName + std::to_string(i),
                                                                       inspect::typescriptSignature(std::vector<std::string>(200, "Float64Array"), "void"),
                                                                       i % 2 == 0,
                                                                       i == 1,
                                                                       std::source_location::current()));
                                      for (const auto& record : owned)
                                          record->fail(std::string(1000, 'e'));

                                      then("a page of one row always fits the smallest budget", [&] {
                                          const auto [rows, next] = registry.page(0, 200, inspect::minPageBytes);
                                          require(!rows.empty());
                                          requireEqual(std::get<1>(rows.front()).size(), inspect::maxTextBytes);
                                          require(next != 0);
                                      });
                                      then("the pages follow each other by the last identifier served, without loss or repetition", [&] {
                                          std::vector<double> ids;
                                          double              after = 0;
                                          int                 pages = 0;
                                          do {
                                              const auto [rows, next] = registry.page(after, 2, inspect::responseBudget<http::DefaultBuffersPolicy>);
                                              require(rows.size() <= 2);
                                              for (const auto& row : rows)
                                                  ids.push_back(std::get<0>(row));
                                              after = next;
                                              ++pages;
                                          } while (after != 0 && pages < 10);
                                          requireEqual(ids, (std::vector<double>{1, 2, 3, 4, 5}));
                                          requireEqual(pages, 3);
                                      });
                                      then("the flags tell the caller and the deferred answer", [&] {
                                          const auto [rows, next] = registry.page(0, 200, inspect::responseBudget<http::DefaultBuffersPolicy>);
                                          static_cast<void>(next);
                                          requireEqual(static_cast<unsigned>(std::get<5>(rows.at(0))), inspect::function_flag::takesCaller);
                                          requireEqual(static_cast<unsigned>(std::get<5>(rows.at(1))), inspect::function_flag::deferred);
                                          requireEqual(std::get<7>(rows.at(0)), 1.0);
                                      });
                                      then("a function released by its owner is listed no more, and the others keep their identifiers", [&] {
                                          owned.erase(owned.begin() + 1);
                                          const auto [rows, next] = registry.page(0, 200, inspect::responseBudget<http::DefaultBuffersPolicy>);
                                          static_cast<void>(next);
                                          requireEqual(rows.size(), std::size_t{4});
                                          requireEqual(std::get<0>(rows.at(1)), 3.0);
                                      });
                                      then("released functions leave the table at once, without any read: it does not grow with what comes and goes", [&] {
                                          requireEqual(registry.size(), std::size_t{5});
                                          for (int i = 0; i < 1000; ++i)
                                              static_cast<void>(
                                                  registry.add("transient", "() => Promise<void>", false, false, std::source_location::current()));
                                          requireEqual(registry.size(), std::size_t{5});
                                          owned.clear();
                                          requireEqual(registry.size(), std::size_t{0});
                                      });
                                      then("a record that outlives its registry is released without it", [&] {
                                          std::shared_ptr<inspect::FunctionRecord> kept;
                                          {
                                              inspect::FunctionRegistry shortLived;
                                              kept = shortLived.add("kept", "() => Promise<void>", false, false, std::source_location::current());
                                          }
                                          requireEqual(kept->name, std::string{"kept"});
                                          kept.reset();
                                      });
                                      then("the application reads the names exactly as it registered them; only a page bounds them", [&] {
                                          const auto functions = registry.list();
                                          requireEqual(functions.size(), std::size_t{5});
                                          requireEqual(functions.at(0).name, longName + "0");
                                          requireEqual(functions.at(4).name, longName + "4");
                                          require(functions.at(0).name != functions.at(4).name);
                                          requireEqual(functions.at(0).label.size(), inspect::maxTextBytes);
                                          require(registry.find(longName + "3").has_value());
                                          const auto control = registry.add("a\nb", "() => Promise<void>", false, false, std::source_location::current());
                                          requireEqual(registry.find("a\nb")->name, std::string{"a\nb"});
                                          requireEqual(registry.find("a\nb")->label,
                                                       std::string{"a\xE2\x90\x8A"
                                                                   "b"});
                                      });
                                      then("a cursor or a count that is not a whole number is refused", [&] {
                                          checkThrows<std::invalid_argument>([&] {
                                              static_cast<void>(registry.page(-1, 200, inspect::minPageBytes));
                                          });
                                          checkThrows<std::invalid_argument>([&] {
                                              static_cast<void>(registry.page(0, 1.5, inspect::minPageBytes));
                                          });
                                      });
                                  }};

const Scenario introSpaces{"The functions of a space are listed with its path, until the space releases them (ADR-027, section 7)", {"inspector"}, [] {
                               IntroBrowser browser;
                               auto&        app = browser.app;
                               app.spaces("/s/", [](std::string_view) -> std::optional<http::RouteReply> {
                                   return std::nullopt;
                               });
                               app.cppFunction("shared", [] { });
                               {
                                   IntroApp::Space space{app, "/s/", "alice"};
                                   space.cppFunction("own", [](int) {
                                       return std::string{};
                                   });
                                   const auto functions = app.registeredFunctions();
                                   const auto own       = std::ranges::find(functions, "own", &inspect::FunctionInfo::name);
                                   then("the space's function names its space, the application's none", [&] {
                                       require(own != functions.end());
                                       requireEqual(own->scope, std::string{"/s/alice/"});
                                       requireEqual(own->signature, std::string{"(a: number) => Promise<string>"});
                                       require(own->location.contains("IntrospectionTests.cpp:"));
                                       require(browser.function("shared").scope.empty());
                                   });
                               }
                               then("once the space is destroyed, its function is listed no more", [&] {
                                   const auto functions = app.registeredFunctions();
                                   require(std::ranges::find(functions, "own", &inspect::FunctionInfo::name) == functions.end());
                                   require(std::ranges::find(functions, "shared", &inspect::FunctionInfo::name) != functions.end());
                               });
                           }};

const Scenario introJsCalls{"The JavaScript functions that C++ calls are counted by name, with the failures of the typed calls (ADR-027, section 7)",
                            {"inspector"},
                            [] {
                                given("the registry of the calls", [&] {
                                    inspect::JsCallRegistry registry;
                                    registry.counter("draw")->called();
                                    registry.counter("draw")->called();
                                    registry.counter("ask")->called();
                                    registry.counter("ask")->failed();
                                    then("each name has one counter, listed in the order of its first call", [&] {
                                        const auto [rows, next] = registry.page(0, 200, inspect::minPageBytes);
                                        requireEqual(next, 0.0);
                                        requireEqual(rows.size(), std::size_t{2});
                                        requireEqual(rows.at(0), (inspect::JsFunctionRow{1.0, "draw", 2.0, 0.0}));
                                        requireEqual(rows.at(1), (inspect::JsFunctionRow{2.0, "ask", 1.0, 1.0}));
                                    });
                                    then("the pages follow each other by identifier", [&] {
                                        const auto [first, next] = registry.page(0, 1, inspect::minPageBytes);
                                        requireEqual(first.size(), std::size_t{1});
                                        requireEqual(next, 1.0);
                                        const auto [second, end] = registry.page(next, 1, inspect::minPageBytes);
                                        requireEqual(std::get<1>(second.at(0)), std::string{"ask"});
                                        requireEqual(end, 0.0);
                                    });
                                    then("past the limit of names, the calls of the others are counted together", [&] {
                                        for (std::size_t i = 0; registry.size() < inspect::maxJsFunctionNames; ++i)
                                            registry.counter("generated-" + std::to_string(i))->called();
                                        registry.counter("one-more")->called();
                                        registry.counter("and-another")->called();
                                        requireEqual(registry.size(), inspect::maxJsFunctionNames + 1);
                                        requireEqual(registry.counter("yet-another")->calls.load(), std::uint64_t{2});
                                        requireEqual(registry.counter("draw")->calls.load(), std::uint64_t{2});
                                    });
                                });
                                given("the source that call objects keep", [&] {
                                    inspect::JsCallSource source;
                                    then("it counts nothing until the inspectors are enabled, then gives the counters of one registry", [&] {
                                        require(source.counter("draw") == nullptr);
                                        require(source.registry() == nullptr);
                                        const auto registry = source.enable();
                                        require(source.enable() == registry);
                                        source.counter("draw")->called();
                                        requireEqual(registry->counter("draw")->calls.load(), std::uint64_t{1});
                                    });
                                });
                                given("an application outside the developer environment", [&] {
                                    IntroBrowser browser;
                                    browser.connect();
                                    then("no call to JavaScript is counted: nothing could read the count", [&] {
                                        require(browser.app.jsCallCounter("draw") == nullptr);
                                    });
                                });
                            }};

const Scenario introBrowsers{"The connected browsers are read page after page, with their pending calls (ADR-027, section 7)", {"inspector"}, [] {
                                 std::vector<inspect::BrowserInfo> browsers(3);
                                 browsers[0].id             = 0;
                                 browsers[0].embeddedView   = true;
                                 browsers[0].localClient    = true;
                                 browsers[0].started        = true;
                                 browsers[0].caller         = true;
                                 browsers[0].path           = "/";
                                 browsers[0].pendingWrites  = 2;
                                 browsers[0].pendingResults = 3;
                                 browsers[0].pendingCalls   = 4;
                                 browsers[1].id             = 1;
                                 browsers[1].path           = std::string(1000, 'p');
                                 browsers[1].space          = "/s/alice/";
                                 browsers[2].id             = 7;
                                 browsers[2].closed         = true;

                                 then("a row tells what the browser is and what it waits for", [&] {
                                     const auto [rows, next, tasks] = inspect::browserPage(browsers, 0, 200, 5, inspect::minPageBytes);
                                     requireEqual(rows.size(), std::size_t{3});
                                     requireEqual(next, 0.0);
                                     requireEqual(tasks, 5.0);
                                     using namespace inspect::browser_flag;
                                     requireEqual(rows.at(0),
                                                  (inspect::BrowserRow{0.0, double{embeddedView | localClient | started | caller}, "/", "", 2.0, 3.0, 4.0}));
                                     requireEqual(std::get<2>(rows.at(1)).size(), inspect::maxTextBytes);
                                     requireEqual(std::get<3>(rows.at(1)), std::string{"/s/alice/"});
                                     requireEqual(static_cast<unsigned>(std::get<1>(rows.at(2))), closed);
                                 });
                                 then("the cursor is the first identifier of the next page: a connection may have the identifier 0", [&] {
                                     const auto [rows, next, tasks] = inspect::browserPage(browsers, 0, 2, 0, inspect::minPageBytes);
                                     static_cast<void>(tasks);
                                     requireEqual(rows.size(), std::size_t{2});
                                     requireEqual(next, 7.0);
                                     const auto [rest, end, ignored] = inspect::browserPage(browsers, next, 2, 0, inspect::minPageBytes);
                                     static_cast<void>(ignored);
                                     requireEqual(rest.size(), std::size_t{1});
                                     requireEqual(std::get<0>(rest.at(0)), 7.0);
                                     requireEqual(end, 0.0);
                                 });
                             }};
