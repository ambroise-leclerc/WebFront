#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <webfront/Commands.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
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
    TaskHandle  spawn(Task task) const;
    auto        jsFunction(string_view name) const;
    auto        writesAtMost(size_t limit) const;
    auto        resumeAfter(chrono::milliseconds) const;
};

// Records the bridge functions that Commands registers, like BasicWF::cppFunction, and the browser calls.
struct FakeApp {
    using UI = FakeUI;

    function<string()>                         list;
    function<void(FakeUI, uint32_t, string, string)> start;
    function<void(FakeUI, uint32_t)>           cancel;
    shared_ptr<detail::UISubscribers<FakeUI>>  closed = make_shared<detail::UISubscribers<FakeUI>>();
    vector<BrowserCall>                        calls;

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
    size_t         backlog = 0;  // frames waiting for the network; each pause lets the browser read 10
    int            pauses  = 0;
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

SCENARIO("Commands register their bridge functions and list the commands") {
    GIVEN("an application with commands") {
        Fixture fixture;
        THEN("the browser discovers them, sorted, with their help") {
            CHECK(fixture.app.registered == vector<string>{"webfront.commands.list", "webfront.commands.start", "webfront.commands.cancel"});
            CHECK(fixture.app.list()
                  == R"([{"name":"code","help":"Exit with 3"},)"
                     R"({"name":"fail","help":"Throw"},{"name":"greet","help":"Greet someone"},{"name":"wait","help":"Wait until stopped"}])");
        }
        THEN("a second Commands object on the application is refused") {
            CHECK_THROWS_AS(Commands<FakeApp>{fixture.app}, logic_error);
        }

        THEN("invalid, reserved and duplicate names are refused") {
            for (const auto* name : {"", "two words", "quo'te", "back\\slash", "tab\t", "wf", "clear", "exit", "help", "history", "greet"})
                CHECK_THROWS_AS(fixture.commands.add(name, "", greet), invalid_argument);
        }
    }
    GIVEN("an application where only webfront.commands.cancel is taken") {
        FakeApp app;
        app.cppFunction<void, FakeUI, uint32_t>("webfront.commands.cancel", [](FakeUI, uint32_t) { });
        THEN("Commands is refused and registers nothing") {
            CHECK_THROWS_AS(Commands<FakeApp>{app}, logic_error);
            CHECK(app.registered == vector<string>{"webfront.commands.cancel"});
        }
    }
}

SCENARIO("A command streams its output and reports its exit") {
    GIVEN("an application with commands") {
        Fixture fixture;
        auto&   app = fixture.app;
        WHEN("a browser runs a command with arguments") {
            app.start(app.ui(1), 7, "greet", R"(["Ada \"Lovelace\"", "é"])");
            THEN("its output, then its exit code, reach that browser") {
                const auto calls = app.callsOf(1);
                REQUIRE(calls.size() == 2);
                CHECK(calls[0].function == "webfront.commands.output");
                CHECK(calls[0].id == 7);
                CHECK(calls[0].values == vector<string>{"Hello Ada \"Lovelace\"\n"});
                CHECK(calls[1].function == "webfront.commands.exit");
                CHECK(calls[1].values == vector<string>{"0", ""});
                CHECK(fixture.commands.running() == 0);
            }
        }
        WHEN("a command throws") {
            app.start(app.ui(1), 1, "fail", "[]");
            THEN("its message is reported with exit code 1") {
                CHECK(app.callsOf(1).back().values == vector<string>{"1", "boom"});
            }
        }
        WHEN("a command sets its exit code") {
            app.start(app.ui(1), 1, "code", "[]");
            THEN("it is reported") {
                CHECK(app.callsOf(1).back().values == vector<string>{"3", ""});
            }
        }
        WHEN("the browser names an unknown command or sends malformed arguments") {
            THEN("its call is rejected and nothing runs") {
                CHECK_THROWS_AS(app.start(app.ui(1), 1, "absent", "[]"), invalid_argument);
                CHECK_THROWS_AS(app.start(app.ui(1), 1, "greet", R"(["unterminated)"), runtime_error);
                CHECK_THROWS_AS(app.start(app.ui(1), 1, "greet", R"({"not":"an array"})"), runtime_error);
                CHECK(app.calls.empty());
                CHECK(fixture.commands.running() == 0);
            }
        }
    }
}

SCENARIO("Command output is split under the bridge string limit on character boundaries") {
    GIVEN("a text of two-byte characters longer than two pieces") {
        string text;
        for (int i = 0; i < 70000; ++i)
            text += "é";
        WHEN("it is split") {
            const auto pieces = commands::detail::chunks(text);
            THEN("each piece is under the limit, ends on a character boundary and the pieces rebuild the text") {
                REQUIRE(pieces.size() == 3);
                string rebuilt;
                for (const auto piece : pieces) {
                    CHECK(piece.size() <= commands::maxChunkBytes);
                    CHECK(piece.size() % 2 == 0);
                    rebuilt += piece;
                }
                CHECK(rebuilt == text);
            }
        }
        WHEN("a command writes it") {
            FakeApp           app;
            Commands<FakeApp> commands{app};
            commands.add("big", "", [text](Context context) -> Task {
                context.write(text);
                co_return;
            });
            app.start(app.ui(1), 1, "big", "[]");
            THEN("the browser receives it in pieces") {
                const auto calls = app.callsOf(1);
                REQUIRE(calls.size() == 4);
                CHECK(calls[0].values[0] + calls[1].values[0] + calls[2].values[0] == text);
                CHECK(calls[3].function == "webfront.commands.exit");
            }
        }
    }
    GIVEN("bytes that are not UTF-8: continuation bytes only") {
        const string bytes(10, '\x80');
        WHEN("they are split") {
            const auto pieces = commands::detail::chunks(bytes, 4);
            THEN("the split still ends, in pieces of the limit") {
                CHECK(pieces == vector<string_view>{string_view(bytes).substr(0, 4), string_view(bytes).substr(4, 4), string_view(bytes).substr(8)});
            }
        }
    }
    GIVEN("short and empty texts") {
        CHECK(commands::detail::chunks("").empty());
        CHECK(commands::detail::chunks("abc") == vector<string_view>{"abc"});
    }
}

SCENARIO("Printing waits while the browser has not read the output") {
    GIVEN("a browser that has more frames to read than allowed") {
        FakeApp           app;
        Commands<FakeApp> commands{app};
        commands.add("print", "", [](Context context) -> Task {
            co_await context.print("line\n");
        });
        app.backlog = 55;
        WHEN("a command prints") {
            app.start(app.ui(1), 1, "print", "[]");
            THEN("it waits once, until no more than the allowed frames are pending, then ends") {
                CHECK(app.pauses == 1);
                CHECK(app.backlog <= commands::maxPendingOutput);
                CHECK(app.callsOf(1).back().values == vector<string>{"0", ""});
            }
        }
    }
}

SCENARIO("Printing a long text waits between its pieces") {
    GIVEN("a browser with as many frames to read as allowed") {
        FakeApp           app;
        Commands<FakeApp> commands{app};
        commands.add("print", "", [](Context context) -> Task {
            co_await context.print(string(2 * commands::maxChunkBytes + 1, 'x'));
        });
        app.backlog  = commands::maxPendingOutput;
        app.queueing = true;
        WHEN("a command prints a text of three pieces") {
            app.start(app.ui(1), 1, "print", "[]");
            THEN("it waits after the piece that overfills the queue, before sending the next") {
                CHECK(app.paced == vector<string>{"send", "pause", "send", "send", "send"});
            }
        }
    }
}

SCENARIO("A print task may be awaited after its context is gone") {
    GIVEN("a command that creates a print task from a context, then reuses its storage for another") {
        FakeApp           app;
        Commands<FakeApp> commands{app};
        commands.add("print", "", [&app](Context context) -> Task {
            optional<Context> temporary{context};
            auto              printing = temporary->print("mine\n");
            temporary.reset();
            temporary.emplace(make_shared<commands::detail::Run<FakeUI>>(app.ui(1), 99, vector<string>{}));
            co_await std::move(printing);
        });
        WHEN("it awaits the task") {
            app.start(app.ui(1), 1, "print", "[]");
            THEN("the text is printed by the command that created the task") {
                const auto calls = app.callsOf(1);
                REQUIRE(calls.size() == 2);
                CHECK(calls[0].id == 1);
                CHECK(calls[0].values == vector<string>{"mine\n"});
            }
        }
    }
}

SCENARIO("Ctrl+C stops a running command and the next one runs at once") {
    GIVEN("a command waiting in a browser") {
        Fixture fixture;
        auto&   app = fixture.app;
        app.start(app.ui(1), 1, "wait", "[]");
        REQUIRE(fixture.commands.running() == 1);
        REQUIRE_FALSE(*fixture.unwound);
        THEN("its number cannot start another command") {
            CHECK_THROWS_AS(app.start(app.ui(1), 1, "greet", "[]"), invalid_argument);
        }
        WHEN("a command that returns once asked to stop is cancelled") {
            fixture.commands.add("polite", "", [](Context context) -> Task {
                co_await (testing::ManualSender([](auto) { }) | networking::ex::upon_stopped([] { return size_t{}; }));
                context.exit(0);
            });
            app.start(app.ui(1), 9, "polite", "[]");
            app.cancel(app.ui(1), 9);
            THEN("it reports exit code 130, not success") {
                CHECK(app.callsOf(1).back().values == vector<string>{"130", ""});
            }
        }
        WHEN("the browser cancels it") {
            app.cancel(app.ui(1), 1);
            THEN("the command unwinds and reports exit code 130") {
                CHECK(*fixture.unwound);
                CHECK(fixture.commands.running() == 0);
                const auto calls = app.callsOf(1);
                CHECK(calls.back().function == "webfront.commands.exit");
                CHECK(calls.back().values == vector<string>{"130", ""});
            }
            AND_WHEN("the browser runs another command and cancels the ended one again") {
                app.start(app.ui(1), 2, "greet", "[]");
                app.cancel(app.ui(1), 1);
                THEN("the new command answers normally") {
                    const auto calls = app.callsOf(1);
                    CHECK(calls.back().id == 2);
                    CHECK(calls.back().values == vector<string>{"0", ""});
                }
            }
        }
    }
}

SCENARIO("Each browser has its own commands") {
    GIVEN("a waiting command in each of two browsers") {
        Fixture fixture;
        auto&   app    = fixture.app;
        auto    second = make_shared<bool>(false);
        fixture.commands.add("wait2", "", [second](Context context) {
            return waitUntilStopped(std::move(context), second);
        });
        app.start(app.ui(1), 1, "wait", "[]");
        app.start(app.ui(2), 1, "wait2", "[]");
        REQUIRE(fixture.commands.running() == 2);
        WHEN("the first browser cancels its command, which has the same number") {
            app.cancel(app.ui(1), 1);
            THEN("the second browser's command keeps running") {
                CHECK(*fixture.unwound);
                CHECK_FALSE(*second);
                CHECK(fixture.commands.running() == 1);
            }
        }
        WHEN("the second browser disconnects") {
            const auto before = app.callsOf(2).size();
            app.disconnect(2);
            THEN("its command is stopped and nothing more is sent to it") {
                CHECK(*second);
                CHECK_FALSE(*fixture.unwound);
                CHECK(fixture.commands.running() == 1);
                CHECK(app.callsOf(2).size() == before);
            }
        }
    }
}

SCENARIO("A browser given the identifier of a disconnected one starts afresh") {
    GIVEN("a command that ignores its stop, in a browser that disconnects") {
        Fixture fixture;
        auto&   app    = fixture.app;
        auto    finish = make_shared<testing::ManualSender::Completion>();
        fixture.commands.add("deaf", "", [finish](Context) -> Task {
            co_await networking::ex::write_env(testing::ManualSender([finish](auto completion) { *finish = std::move(completion); }),
                                               networking::ex::prop{networking::ex::get_stop_token, networking::ex::never_stop_token{}});
        });
        app.start(app.ui(2), 1, "deaf", "[]");
        app.disconnect(2);
        WHEN("a new browser with the same identifier runs a command with the same number") {
            app.start(app.ui(2), 1, "greet", R"(["again"])");
            THEN("it runs, and only its own messages reach the new browser") {
                const auto calls = app.callsOf(2);
                REQUIRE(calls.size() == 2);
                CHECK(calls[0].values == vector<string>{"Hello again\n"});
                CHECK(calls[1].values == vector<string>{"0", ""});
            }
            AND_WHEN("the deaf command ends at last") {
                finish->complete({}, 0);
                THEN("its exit report does not reach the new browser either") {
                    CHECK(app.callsOf(2).size() == 2);
                }
            }
        }
        if (finish->complete)
            finish->complete({}, 0);  // lets the shutdown drain it
    }
}

SCENARIO("Shutdown drains running commands") {
    GIVEN("a waiting command") {
        Fixture fixture;
        auto&   app = fixture.app;
        app.start(app.ui(1), 1, "wait", "[]");
        WHEN("the application shuts down") {
            app.real.shutdown();
            THEN("the command unwound before shutdown returned") {
                CHECK(*fixture.unwound);
                CHECK(fixture.commands.running() == 0);
            }
            AND_WHEN("a browser starts another command") {
                THEN("its call is rejected and nothing is left running") {
                    CHECK_THROWS_AS(app.start(app.ui(1), 2, "greet", "[]"), runtime_error);
                    CHECK(fixture.commands.running() == 0);
                }
            }
        }
    }
}

SCENARIO("A command may take its context by reference") {
    GIVEN("a command whose coroutine keeps a reference to its context across a suspension") {
        FakeApp           app;
        Commands<FakeApp> commands{app};
        commands.add("late", "", [](const Context& context) -> Task {
            co_await context.ui().resumeAfter(chrono::milliseconds(1));
            context.write("after the pause\n");
        });
        WHEN("it runs") {
            app.start(app.ui(1), 1, "late", "[]");
            THEN("its context is still valid after the suspension") {
                CHECK(app.callsOf(1).front().values == vector<string>{"after the pause\n"});
            }
        }
    }
}

SCENARIO("A copy of a context may outlive its command") {
    GIVEN("a command that keeps a copy of its context") {
        FakeApp                  app;
        Commands<FakeApp>        commands{app};
        auto                     kept = make_shared<optional<Context>>();
        commands.add("keep", "", [kept](Context context) -> Task {
            kept->emplace(context);
            co_return;
        });
        app.start(app.ui(1), 1, "keep", "[]");
        WHEN("the command has ended") {
            THEN("the copy still reads its stop state, and writes nothing") {
                REQUIRE(kept->has_value());
                CHECK_FALSE((*kept)->stopRequested());
                CHECK_FALSE((*kept)->stopToken().stop_requested());
                const auto calls = app.callsOf(1).size();
                (*kept)->write("late\n");
                CHECK(app.callsOf(1).size() == calls + 1);
            }
        }
    }
}

SCENARIO("Commands may outlive their registering object") {
    GIVEN("commands whose Commands object is gone") {
        FakeApp app;
        {
            Commands<FakeApp> commands{app};
            commands.add("greet", "", greet);
        }
        THEN("the browser still runs them") {
            app.start(app.ui(1), 1, "greet", R"(["you"])");
            CHECK(app.callsOf(1).front().values == vector<string>{"Hello you\n"});
        }
    }
}

SCENARIO("BasicWF provides the application facilities of commands") {
    GIVEN("an application over the networking mock") {
        RealApp app(WebFrontConfig{.port = "0"});
        THEN("browsers have identifiers and disconnections can be subscribed to") {
            CHECK(RealApp::UI(app, 5).id() == 5);
            auto subscription = app.subscribeUIClosed([](RealApp::UI) { });
            CHECK(subscription.active());
        }
        THEN("a UI is a regular value usable in containers") {
            static_assert(std::is_copy_assignable_v<RealApp::UI>);
            static_assert(std::is_nothrow_move_assignable_v<RealApp::UI>);
            static_assert(std::equality_comparable<RealApp::UI>);
            std::vector<RealApp::UI> clients{RealApp::UI(app, 1), RealApp::UI(app, 2), RealApp::UI(app, 3)};
            std::erase_if(clients, [](const RealApp::UI& ui) { return ui.id() == 2; });
            REQUIRE(clients.size() == 2);
            CHECK(clients[0] == RealApp::UI(app, 1));
            CHECK(clients[1].id() == 3);
            std::optional<RealApp::UI> current;
            current = clients[1];
            CHECK(current->id() == 3);
        }
        THEN("a second Commands object is refused, and cppFunction tells a taken name") {
            Commands<RealApp> commands{app};
            CHECK_THROWS_AS(Commands<RealApp>{app}, logic_error);
            CHECK_FALSE(app.cppFunction("webfront.commands.list", [] { return 0; }));
            CHECK(app.cppFunction("another", [] { return 0; }));
        }
        THEN("calls to a browser that is gone are dropped, and typed calls fail") {
            const RealApp::UI gone(app, 42);
            CHECK(app.connectedBrowsers() == 0);
            CHECK_NOTHROW(gone.jsFunction("console.log")("dropped"));
            auto result = gone.jsFunction<int>("Math.abs")(-1);
            CHECK_THROWS_WITH(result.get(), "Browser connection closed");
            CHECK_NOTHROW(gone.jsFunctionSender<int>("Math.abs")(-1));
        }
    }
}
