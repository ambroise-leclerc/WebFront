// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-22
//
// WebFront — tests/unit
// Role: Terminal commands of an application: discovery, output streaming, cancellation and reserved names.
// Thread: bridge-io | Alloc: unrestricted
// LLM: uses a fake application

#include "InjectableNetworking.hpp"
#include <webfront/Commands.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

namespace {

using RealApp = BasicWF<networking::NetworkingMock, fs::DefaultFS>;

struct FakeApp;

// A browser call made by the commands: its function, the command number and the other values as text.
struct BrowserCall {
    int            link;
    string         function;
    uint32_t       id;
    vector<string> values;
};

template <typename T>
string text(const T& value) {
    if constexpr (is_arithmetic_v<T>)
        return to_string(value);
    else
        return string(value);
}

// The browser side of a FakeApp: spawns on a real application over the networking mock, whose tasks start inline,
// and records the browser calls instead of sending them.
struct FakeUI {
    FakeApp* app;
    int      link;

    [[nodiscard]] int id() const noexcept {
        return link;
    }
    TaskHandle spawn(Task task) const;
    auto       jsFunction(string_view name) const;
    auto       writesAtMost(size_t limit) const;
    auto       resumeAfter(chrono::milliseconds) const;
};

// Records the bridge functions that Commands registers, like BasicWF::cppFunction, and the browser calls.
struct FakeApp {
    using UI = FakeUI;

    function<string()>                                  list;
    function<void(FakeUI, uint32_t, string, string)>    start;
    function<void(FakeUI, uint32_t)>                    cancel;
    shared_ptr<webfront::detail::UISubscribers<FakeUI>> closed = make_shared<webfront::detail::UISubscribers<FakeUI>>();
    vector<BrowserCall>                                 calls;

    [[nodiscard]] bool hasCppFunction(string_view name) const {
        return std::ranges::find(registered, name) != registered.end();
    }

    template <typename R, typename... Args>
    bool cppFunction(string name, auto&& function) {
        if (std::ranges::find(registered, name) != registered.end())
            return false;
        if constexpr (sizeof...(Args) == 0)
            list = std::forward<decltype(function)>(function);
        else if constexpr (sizeof...(Args) == 2)
            cancel = std::forward<decltype(function)>(function);
        else
            start = std::forward<decltype(function)>(function);
        registered.push_back(std::move(name));
        return true;
    }

    UISubscription subscribeUIClosed(function<void(FakeUI)> handler) {
        return closed->subscribe(std::move(handler));
    }

    FakeUI ui(int link) {
        return FakeUI{this, link};
    }

    // The calls to a disconnected browser are still recorded, as if a later browser had been given its identifier.
    void disconnect(int link) {
        closed->notify(ui(link));
    }

    [[nodiscard]] vector<BrowserCall> callsOf(int link) const {
        vector<BrowserCall> selected;
        for (const auto& call : calls)
            if (call.link == link)
                selected.push_back(call);
        return selected;
    }

    vector<string> registered;
    size_t         backlog  = 0;      // frames waiting for the network; each pause lets the browser read 10
    int            pauses   = 0;
    bool           queueing = false;  // each browser call adds a frame to the backlog
    vector<string> paced;             // "send" and "pause", in order
    // Destroyed first: draining its tasks lets the commands report through the members above.
    RealApp real{WebFrontConfig{.port = "0"}};
};

TaskHandle FakeUI::spawn(Task task) const {
    return app->real.spawn(std::move(task));
}

// Completes at once when the queue is short enough; else the browser reads until it is down to half the limit.
auto FakeUI::writesAtMost(size_t limit) const {
    return networking::ex::just() | networking::ex::then([app = app, limit] {
               if (app->backlog <= limit)
                   return;
               ++app->pauses;
               app->paced.push_back("pause");
               app->backlog = min(app->backlog, limit / 2);
           });
}

auto FakeUI::resumeAfter(chrono::milliseconds) const {
    return networking::ex::just();
}

auto FakeUI::jsFunction(string_view name) const {
    return [app = app, link = link, function = string(name)](uint32_t id, const auto&... values) {
        app->calls.push_back(BrowserCall{link, function, id, {text(values)...}});
        if (app->queueing) {
            ++app->backlog;
            app->paced.push_back("send");
        }
    };
}

using Context = commands::Context<FakeUI>;

// Unwinding sets its flag: the command observed its cancellation.
struct Unwound {
    shared_ptr<bool> flag;
    ~Unwound() {
        *flag = true;
    }
};

Task greet(Context context) {
    context.write("Hello " + (context.args().empty() ? string{"world"} : context.args()[0]) + "\n");
    co_return;
}

Task waitUntilStopped(Context context, shared_ptr<bool> unwound) {
    Unwound guard{std::move(unwound)};
    context.write("waiting\n");
    co_await testing::ManualSender([](auto) { });
    context.write("never reached\n");
}

// A fixture with commands of every kind.
struct Fixture {
    FakeApp           app;
    Commands<FakeApp> commands{app};
    shared_ptr<bool>  unwound = make_shared<bool>(false);

    Fixture() {
        commands.add("greet", "Greet someone", greet);
        commands.add("wait", "Wait until stopped", [unwound = unwound](Context context) {
            return waitUntilStopped(std::move(context), unwound);
        });
        commands.add("fail", "Throw", [](Context) -> Task {
            throw runtime_error("boom");
        });
        commands.add("code", "Exit with 3", [](Context context) -> Task {
            context.exit(3);
            co_return;
        });
    }
};

}  // namespace

const Scenario commandsRegister{
    "Commands register their bridge functions and list the commands",
    [] {
        given("an application with commands", [&] {
            Fixture fixture;
            then("the browser discovers them, sorted, with their help", [&] {
                check(fixture.app.registered == vector<string>{"webfront.commands.list", "webfront.commands.start", "webfront.commands.cancel"});
                check(fixture.app.list()
                      == R"([{"name":"code","help":"Exit with 3"},)"
                         R"({"name":"fail","help":"Throw"},{"name":"greet","help":"Greet someone"},{"name":"wait","help":"Wait until stopped"}])");
            });
            then("a second Commands object on the application is refused", [&] {
                checkThrows<logic_error>([&] {
                    Commands<FakeApp>{fixture.app};
                });
            });

            then("invalid, reserved and duplicate names are refused", [&] {
                for (const auto* name : {"", "two words", "quo'te", "back\\slash", "tab\t", "wf", "clear", "exit", "help", "history", "greet"})
                    checkThrows<invalid_argument>([&] {
                        fixture.commands.add(name, "", greet);
                    });
            });
            then("every name the terminal keeps for itself is refused, the file commands it does not provide included", [&] {
                for (const auto name : webfront::commands::reservedNames)
                    checkThrows<invalid_argument>([&] {
                        fixture.commands.add(std::string(name), "", greet);
                    });
                for (const auto* name : {"ls", "cat", "tree", "mounts", "rm", "cd", "edit"})
                    check(std::ranges::find(webfront::commands::reservedNames, std::string_view{name}) != webfront::commands::reservedNames.end());
            });
        });
        given("an application where only webfront.commands.cancel is taken", [&] {
            FakeApp app;
            app.cppFunction<void, FakeUI, uint32_t>("webfront.commands.cancel", [](FakeUI, uint32_t) { });
            then("Commands is refused and registers nothing", [&] {
                checkThrows<logic_error>([&] {
                    Commands<FakeApp>{app};
                });
                check(app.registered == vector<string>{"webfront.commands.cancel"});
            });
        });
    }};

const Scenario commandStreams{"A command streams its output and reports its exit", [] {
                                  given("an application with commands", [&] {
                                      Fixture fixture;
                                      auto&   app = fixture.app;
                                      when("a browser runs a command with arguments", [&] {
                                          app.start(app.ui(1), 7, "greet", R"(["Ada \"Lovelace\"", "é"])");
                                          then("its output, then its exit code, reach that browser", [&] {
                                              const auto calls = app.callsOf(1);
                                              require(calls.size() == 2);
                                              check(calls[0].function == "webfront.commands.output");
                                              check(calls[0].id == 7);
                                              check(calls[0].values == vector<string>{"Hello Ada \"Lovelace\"\n"});
                                              check(calls[1].function == "webfront.commands.exit");
                                              check(calls[1].values == vector<string>{"0", ""});
                                              check(fixture.commands.running() == 0);
                                          });
                                      });
                                      when("a command throws", [&] {
                                          app.start(app.ui(1), 1, "fail", "[]");
                                          then("its message is reported with exit code 1", [&] {
                                              check(app.callsOf(1).back().values == vector<string>{"1", "boom"});
                                          });
                                      });
                                      when("a command sets its exit code", [&] {
                                          app.start(app.ui(1), 1, "code", "[]");
                                          then("it is reported", [&] {
                                              check(app.callsOf(1).back().values == vector<string>{"3", ""});
                                          });
                                      });
                                      when("the browser names an unknown command or sends malformed arguments", [&] {
                                          then("its call is rejected and nothing runs", [&] {
                                              checkThrows<invalid_argument>([&] {
                                                  app.start(app.ui(1), 1, "absent", "[]");
                                              });
                                              checkThrows<runtime_error>([&] {
                                                  app.start(app.ui(1), 1, "greet", R"(["unterminated)");
                                              });
                                              checkThrows<runtime_error>([&] {
                                                  app.start(app.ui(1), 1, "greet", R"({"not":"an array"})");
                                              });
                                              check(app.calls.empty());
                                              check(fixture.commands.running() == 0);
                                          });
                                      });
                                  });
                              }};

const Scenario commandOutput{
    "Command output is split under the bridge string limit on character boundaries",
    [] {
        given("a text of two-byte characters longer than two pieces", [&] {
            string text;
            for (int i = 0; i < 70000; ++i)
                text += "é";
            when("it is split", [&] {
                const auto pieces = commands::detail::chunks(text);
                then("each piece is under the limit, ends on a character boundary and the pieces rebuild the text", [&] {
                    require(pieces.size() == 3);
                    string rebuilt;
                    for (const auto piece : pieces) {
                        check(piece.size() <= commands::maxChunkBytes);
                        check(piece.size() % 2 == 0);
                        rebuilt += piece;
                    }
                    check(rebuilt == text);
                });
            });
            when("a command writes it", [&] {
                FakeApp           app;
                Commands<FakeApp> commands{app};
                commands.add("big", "", [text](Context context) -> Task {
                    context.write(text);
                    co_return;
                });
                app.start(app.ui(1), 1, "big", "[]");
                then("the browser receives it in pieces", [&] {
                    const auto calls = app.callsOf(1);
                    require(calls.size() == 4);
                    check(calls[0].values[0] + calls[1].values[0] + calls[2].values[0] == text);
                    check(calls[3].function == "webfront.commands.exit");
                });
            });
        });
        given("bytes that are not UTF-8: continuation bytes only", [&] {
            const string bytes(10, '\x80');
            when("they are split", [&] {
                const auto pieces = commands::detail::chunks(bytes, 4);
                then("the split still ends, in pieces of the limit", [&] {
                    check(pieces == vector<string_view>{string_view(bytes).substr(0, 4), string_view(bytes).substr(4, 4), string_view(bytes).substr(8)});
                });
            });
        });
        given("short and empty texts", [&] {
            check(commands::detail::chunks("").empty());
            check(commands::detail::chunks("abc") == vector<string_view>{"abc"});
        });
    }};

const Scenario printingWaits{"Printing waits while the browser has not read the output", [] {
                                 given("a browser that has more frames to read than allowed", [&] {
                                     FakeApp           app;
                                     Commands<FakeApp> commands{app};
                                     commands.add("print", "", [](Context context) -> Task {
                                         co_await context.print("line\n");
                                     });
                                     app.backlog = 55;
                                     when("a command prints", [&] {
                                         app.start(app.ui(1), 1, "print", "[]");
                                         then("it waits once, until no more than the allowed frames are pending, then ends", [&] {
                                             check(app.pauses == 1);
                                             check(app.backlog <= commands::maxPendingOutput);
                                             check(app.callsOf(1).back().values == vector<string>{"0", ""});
                                         });
                                     });
                                 });
                             }};

const Scenario printingLong{"Printing a long text waits between its pieces", [] {
                                given("a browser with as many frames to read as allowed", [&] {
                                    FakeApp           app;
                                    Commands<FakeApp> commands{app};
                                    commands.add("print", "", [](Context context) -> Task {
                                        co_await context.print(string(2 * commands::maxChunkBytes + 1, 'x'));
                                    });
                                    app.backlog  = commands::maxPendingOutput;
                                    app.queueing = true;
                                    when("a command prints a text of three pieces", [&] {
                                        app.start(app.ui(1), 1, "print", "[]");
                                        then("it waits after the piece that overfills the queue, before sending the next", [&] {
                                            check(app.paced == vector<string>{"send", "pause", "send", "send", "send"});
                                        });
                                    });
                                });
                            }};

const Scenario printTask{"A print task may be awaited after its context is gone", [] {
                             given("a command that creates a print task from a context, then reuses its storage for another", [&] {
                                 FakeApp           app;
                                 Commands<FakeApp> commands{app};
                                 commands.add("print", "", [&app](Context context) -> Task {
                                     optional<Context> temporary{context};
                                     auto              printing = temporary->print("mine\n");
                                     temporary.reset();
                                     temporary.emplace(make_shared<commands::detail::Run<FakeUI>>(app.ui(1), 99, vector<string>{}));
                                     co_await std::move(printing);
                                 });
                                 when("it awaits the task", [&] {
                                     app.start(app.ui(1), 1, "print", "[]");
                                     then("the text is printed by the command that created the task", [&] {
                                         const auto calls = app.callsOf(1);
                                         require(calls.size() == 2);
                                         check(calls[0].id == 1);
                                         check(calls[0].values == vector<string>{"mine\n"});
                                     });
                                 });
                             });
                         }};

const Scenario ctrlC{"An interruption stops a running command and the next one runs at once", [] {
                         given("a command waiting in a browser", [&] {
                             Fixture fixture;
                             auto&   app = fixture.app;
                             app.start(app.ui(1), 1, "wait", "[]");
                             require(fixture.commands.running() == 1);
                             require(!(*fixture.unwound));
                             then("its number cannot start another command", [&] {
                                 checkThrows<invalid_argument>([&] {
                                     app.start(app.ui(1), 1, "greet", "[]");
                                 });
                             });
                             when("a command that returns once asked to stop is cancelled", [&] {
                                 fixture.commands.add("polite", "", [](Context context) -> Task {
                                     co_await (testing::ManualSender([](auto) { }) | networking::ex::upon_stopped([] {
                                                   return size_t{};
                                               }));
                                     context.exit(0);
                                 });
                                 app.start(app.ui(1), 9, "polite", "[]");
                                 app.cancel(app.ui(1), 9);
                                 then("it reports exit code 130, not success", [&] {
                                     check(app.callsOf(1).back().values == vector<string>{"130", ""});
                                 });
                             });
                             when("the browser cancels it", [&] {
                                 app.cancel(app.ui(1), 1);
                                 then("the command unwinds and reports exit code 130", [&] {
                                     check(*fixture.unwound);
                                     check(fixture.commands.running() == 0);
                                     const auto calls = app.callsOf(1);
                                     check(calls.back().function == "webfront.commands.exit");
                                     check(calls.back().values == vector<string>{"130", ""});
                                 });
                                 andWhen("the browser runs another command and cancels the ended one again", [&] {
                                     app.start(app.ui(1), 2, "greet", "[]");
                                     app.cancel(app.ui(1), 1);
                                     then("the new command answers normally", [&] {
                                         const auto calls = app.callsOf(1);
                                         check(calls.back().id == 2);
                                         check(calls.back().values == vector<string>{"0", ""});
                                     });
                                 });
                             });
                         });
                     }};

const Scenario eachBrowser{"Each browser has its own commands", [] {
                               given("a waiting command in each of two browsers", [&] {
                                   Fixture fixture;
                                   auto&   app    = fixture.app;
                                   auto    second = make_shared<bool>(false);
                                   fixture.commands.add("wait2", "", [second](Context context) {
                                       return waitUntilStopped(std::move(context), second);
                                   });
                                   app.start(app.ui(1), 1, "wait", "[]");
                                   app.start(app.ui(2), 1, "wait2", "[]");
                                   require(fixture.commands.running() == 2);
                                   when("the first browser cancels its command, which has the same number", [&] {
                                       app.cancel(app.ui(1), 1);
                                       then("the second browser's command keeps running", [&] {
                                           check(*fixture.unwound);
                                           check(!(*second));
                                           check(fixture.commands.running() == 1);
                                       });
                                   });
                                   when("the second browser disconnects", [&] {
                                       const auto before = app.callsOf(2).size();
                                       app.disconnect(2);
                                       then("its command is stopped and nothing more is sent to it", [&] {
                                           check(*second);
                                           check(!(*fixture.unwound));
                                           check(fixture.commands.running() == 1);
                                           check(app.callsOf(2).size() == before);
                                       });
                                   });
                               });
                           }};

const Scenario browserGiven{"A browser given the identifier of a disconnected one starts afresh", [] {
                                given("a command that ignores its stop, in a browser that disconnects", [&] {
                                    Fixture fixture;
                                    auto&   app    = fixture.app;
                                    auto    finish = make_shared<testing::ManualSender::Completion>();
                                    fixture.commands.add("deaf", "", [finish](Context) -> Task {
                                        co_await networking::ex::write_env(
                                            testing::ManualSender([finish](auto completion) {
                                                *finish = std::move(completion);
                                            }),
                                            networking::ex::prop{networking::ex::get_stop_token, networking::ex::never_stop_token{}});
                                    });
                                    app.start(app.ui(2), 1, "deaf", "[]");
                                    app.disconnect(2);
                                    when("a new browser with the same identifier runs a command with the same number", [&] {
                                        app.start(app.ui(2), 1, "greet", R"(["again"])");
                                        then("it runs, and only its own messages reach the new browser", [&] {
                                            const auto calls = app.callsOf(2);
                                            require(calls.size() == 2);
                                            check(calls[0].values == vector<string>{"Hello again\n"});
                                            check(calls[1].values == vector<string>{"0", ""});
                                        });
                                        andWhen("the deaf command ends at last", [&] {
                                            finish->complete({}, 0);
                                            then("its exit report does not reach the new browser either", [&] {
                                                check(app.callsOf(2).size() == 2);
                                            });
                                        });
                                    });
                                    if (finish->complete)
                                        finish->complete({}, 0);  // lets the shutdown drain it
                                });
                            }};

const Scenario shutdownDrains{"Shutdown drains running commands", [] {
                                  given("a waiting command", [&] {
                                      Fixture fixture;
                                      auto&   app = fixture.app;
                                      app.start(app.ui(1), 1, "wait", "[]");
                                      when("the application shuts down", [&] {
                                          app.real.shutdown();
                                          then("the command unwound before shutdown returned", [&] {
                                              check(*fixture.unwound);
                                              check(fixture.commands.running() == 0);
                                          });
                                          andWhen("a browser starts another command", [&] {
                                              then("its call is rejected and nothing is left running", [&] {
                                                  checkThrows<runtime_error>([&] {
                                                      app.start(app.ui(1), 2, "greet", "[]");
                                                  });
                                                  check(fixture.commands.running() == 0);
                                              });
                                          });
                                      });
                                  });
                              }};

const Scenario commandMay{"A command may take its context by reference", [] {
                              given("a command whose coroutine keeps a reference to its context across a suspension", [&] {
                                  FakeApp           app;
                                  Commands<FakeApp> commands{app};
                                  commands.add("late", "", [](const Context& context) -> Task {
                                      co_await context.ui().resumeAfter(chrono::milliseconds(1));
                                      context.write("after the pause\n");
                                  });
                                  when("it runs", [&] {
                                      app.start(app.ui(1), 1, "late", "[]");
                                      then("its context is still valid after the suspension", [&] {
                                          check(app.callsOf(1).front().values == vector<string>{"after the pause\n"});
                                      });
                                  });
                              });
                          }};

const Scenario copyContext{"A copy of a context may outlive its command", [] {
                               given("a command that keeps a copy of its context", [&] {
                                   FakeApp           app;
                                   Commands<FakeApp> commands{app};
                                   auto              kept = make_shared<optional<Context>>();
                                   commands.add("keep", "", [kept](Context context) -> Task {
                                       kept->emplace(context);
                                       co_return;
                                   });
                                   app.start(app.ui(1), 1, "keep", "[]");
                                   when("the command has ended", [&] {
                                       then("the copy still reads its stop state, and writes nothing", [&] {
                                           require(kept->has_value());
                                           check(!((*kept)->stopRequested()));
                                           check(!((*kept)->stopToken().stop_requested()));
                                           const auto calls = app.callsOf(1).size();
                                           (*kept)->write("late\n");
                                           check(app.callsOf(1).size() == calls + 1);
                                       });
                                   });
                               });
                           }};

const Scenario commandsMay{"Commands may outlive their registering object", [] {
                               given("commands whose Commands object is gone", [&] {
                                   FakeApp app;
                                   {
                                       Commands<FakeApp> commands{app};
                                       commands.add("greet", "", greet);
                                   }
                                   then("the browser still runs them", [&] {
                                       app.start(app.ui(1), 1, "greet", R"(["you"])");
                                       check(app.callsOf(1).front().values == vector<string>{"Hello you\n"});
                                   });
                               });
                           }};

const Scenario basicWFProvides{"BasicWF provides the application facilities of commands", [] {
                                   given("an application over the networking mock", [&] {
                                       RealApp app(WebFrontConfig{.port = "0"});
                                       then("browsers have identifiers and disconnections can be subscribed to", [&] {
                                           check(RealApp::UI(app, 5, 1).id() == 5);
                                           auto subscription = app.subscribeUIClosed([](RealApp::UI) { });
                                           check(subscription.active());
                                       });
                                       then("a second Commands object is refused, and cppFunction tells a taken name", [&] {
                                           Commands<RealApp> commands{app};
                                           checkThrows<logic_error>([&] {
                                               Commands<RealApp>{app};
                                           });
                                           check(!app.cppFunction("webfront.commands.list", [] {
                                               return 0;
                                           }));
                                           check(app.cppFunction("another", [] {
                                               return 0;
                                           }));
                                       });
                                       then("calls to a browser that is gone are dropped, and typed calls fail", [&] {
                                           const RealApp::UI gone(app, 42, 1);
                                           check(app.connectedBrowsers() == 0);
                                           checkNothrow([&] {
                                               gone.jsFunction("console.log")("dropped");
                                           });
                                           auto result = gone.jsFunction<int>("Math.abs")(-1);
                                           checkThrows(
                                               [&] {
                                                   (void)result.get();
                                               },
                                               equals("Browser connection closed"));
                                           checkNothrow([&] {
                                               gone.jsFunctionSender<int>("Math.abs")(-1);
                                           });
                                       });
                                   });
                               }};
