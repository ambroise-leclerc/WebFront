// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — tests/unit
// Role: A distribution build has no inspector function, even with the developer environment bootstrapped (ADR-027).
// Thread: ui-loop | Alloc: heap-init
// LLM: compiled with WEBFRONT_DISTRIBUTION=1 by the InspectorsAbsentFromDistribution test; exit status is the verdict

#include "TemporaryDirectory.hpp"
#include <webfront/DeveloperApp.hpp>
#include <webfront/testing/Bdd.hpp>

#if !(defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION)
    #error "This test checks a distribution build: compile it with WEBFRONT_DISTRIBUTION=1"
#endif

using namespace webfront::bdd;
using namespace webfront;

namespace {
struct QuietFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
};
}  // namespace

const Scenario inspectorsAbsent{"A distribution build registers no inspector function (ADR-027, section 1)", [] {
                                    given("a developer application bootstrapped in a distribution build", [&] {
                                        webfront::test::TemporaryDirectory project;
                                        BasicDeveloperApp<QuietFrontend>   app(project.path(), WebFrontConfig{.port = "0"});
                                        app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"});
                                        then("the environment's callbacks exist, and no webfront.inspector.* function", [&] {
                                            require(app.hasCppFunction("webfront.developer.config"));
                                            for (const auto* name : {"webfront.inspector.containers",
                                                                     "webfront.inspector.embedded",
                                                                     "webfront.inspector.watches",
                                                                     "webfront.inspector.functions",
                                                                     "webfront.inspector.jsFunctions",
                                                                     "webfront.inspector.browsers",
                                                                     "webfront.inspector.bridgeSource",
                                                                     "webfront.inspector.publishBridge",
                                                                     "webfront.inspector.bridge",
                                                                     "webfront.inspector.readPreferences",
                                                                     "webfront.inspector.savePreferences",
                                                                     "webfront.inspector.openWindow",
                                                                     "webfront.inspector.closeWindow",
                                                                     "webfront.inspector.limits",
                                                                     "webfront.inspector.children",
                                                                     "webfront.inspector.changes",
                                                                     "webfront.inspector.value",
                                                                     "webfront.inspector.fold",
                                                                     "webfront.inspector.reset",
                                                                     "webfront.inspector.write"})
                                                require(!app.hasCppFunction(name));
                                        });
                                        then("native code gets no reads of the inspectors either (ADR-027, section 10)", [&] {
                                            require(app.inspectorReads() == nullptr);
                                        });
                                        then("nothing is recorded of the functions or of the calls to JavaScript", [&] {
                                            app.cppFunction("ping", [](int value) {
                                                return value;
                                            });
                                            require(app.hasCppFunction("ping"));
                                            require(app.registeredFunctions().empty());
                                            require(app.jsCallCounter("draw") == nullptr);
                                        });
                                        then("a watch registers nothing: its handle is empty", [&] {
                                            int  value = 0;
                                            auto watch = app.watch("value", [&value]() -> const int& {
                                                return value;
                                            });
                                            require(!watch);
                                        });
                                    });
                                }};
