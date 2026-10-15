#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <components/Terminal.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace std;
using namespace webfront;

namespace {

// Records the functions a Terminal registers, like BasicWF::cppFunction, and calls them directly.
struct RecordingApp {
    using LineResult = tuple<string, string, string>;

    map<string, function<string()>>       nullary;
    map<string, function<string(string)>> unary;
    function<LineResult(string)>          runLine;

    template <typename R, typename... Args>
    void cppFunction(string name, auto&& function) {
        if constexpr (is_same_v<R, LineResult>)
            runLine = std::forward<decltype(function)>(function);
        else if constexpr (sizeof...(Args) == 0)
            nullary.try_emplace(std::move(name), std::forward<decltype(function)>(function));
        else
            unary.try_emplace(std::move(name), std::forward<decltype(function)>(function));
    }

    string run(const string& line) {
        return get<0>(runLine(line));
    }
    string complete(const string& line) {
        return unary.at("webfront.terminal.complete")(line);
    }
};

bool contains(const string& text, const string& part) {
    return text.find(part) != string::npos;
}

// A shell with one command per accepted handler shape, plus a throwing one.
struct SampleShell {
    terminal::Shell shell;
    vector<string>  received;

    SampleShell() {
        shell.command("greet", "Greet someone", terminal::adaptHandler([](CommandArgs args) {
                          return "Hello, " + (args.empty() ? string{"world"} : args[0]) + "!";
                      }));
        shell.command("record", "Record its arguments", terminal::adaptHandler([this](CommandArgs args) {
                          received.assign(args.begin(), args.end());
                      }));
        shell.command("lines", "Write two lines", terminal::adaptHandler([](CommandArgs, ostream& output) {
                          output << "one\ntwo";
                      }));
        shell.command("answer", "No argument", terminal::adaptHandler([] {
                          return 42;
                      }));
        shell.command("fail", "Throw", terminal::adaptHandler([](CommandArgs) -> string {
                          throw runtime_error("command failed");
                      }));
    }
    SampleShell(const SampleShell&)            = delete;
    SampleShell& operator=(const SampleShell&) = delete;
};

}  // namespace

SCENARIO("Terminal lines are split into words") {
    GIVEN("lines with whitespace, quotes and escapes") {
        auto [line, words] = GENERATE(table<string, vector<string>>({
            {          "  greet \t Ada  ",              {"greet", "Ada"}}, // whitespace runs separate words
            {                          "",                            {}}, // an empty line has no word
            {                       "   ",                            {}}, // nor has a blank one
            {R"(say "hello world" 'a b')", {"say", "hello world", "a b"}},
            {                 R"(x "" y)",                {"x", "", "y"}}, // empty quotes give an empty word
            {           R"(pre"fix"post)",                {"prefixpost"}}, // quotes join adjacent text
            {                   R"(a\ b)",                       {"a b"}}, // a backslash escapes outside quotes,
            {           R"("say \"hi\"")",               {R"(say "hi")"}}, // inside double quotes,
            {                  R"('a\b')",                    {R"(a\b)"}}, // but not inside single quotes
        }));
        THEN("the line '" + line + "' is split as expected") {
            REQUIRE(terminal::tokenize(line) == words);
        }
    }
    GIVEN("malformed lines") {
        auto line = GENERATE(string{R"(say "hello)"}, string{"say 'hello"}, string{R"(say \)"}, string{R"("a\)"});
        THEN("an unterminated quote or a trailing backslash is rejected: " + line) {
            REQUIRE_THROWS_AS(terminal::tokenize(line), invalid_argument);
        }
    }
}

SCENARIO("The terminal shell runs commands of every accepted handler shape") {
    GIVEN("a shell with sample commands") {
        SampleShell sample;
        auto&       shell = sample.shell;

        THEN("a returning command's result is its output") {
            REQUIRE(shell.run("greet Ada") == "Hello, Ada!");
            REQUIRE(shell.run("greet") == "Hello, world!");
            REQUIRE(shell.run("answer ignored") == "42");
        }
        THEN("the command receives its arguments without its name") {
            REQUIRE(shell.run("record 'a b' c").empty());
            REQUIRE(sample.received == vector<string>{"a b", "c"});
        }
        THEN("written line breaks become terminal line breaks") {
            REQUIRE(shell.run("lines") == "one\r\ntwo");
        }
        THEN("an empty line produces no output") {
            REQUIRE(shell.run("").empty());
            REQUIRE(shell.run("   ").empty());
        }
    }
}

SCENARIO("The terminal shell reports failing lines") {
    GIVEN("a shell with sample commands") {
        SampleShell sample;
        auto&       shell = sample.shell;

        THEN("an unknown command or a malformed line produces an error message, not an exception") {
            const auto unknown = shell.run("nope");
            REQUIRE(contains(unknown, "Unknown command 'nope'"));
            REQUIRE(unknown.starts_with("\x1b[31m"));
            REQUIRE(contains(shell.run(R"(greet "Ada)"), "Unterminated quote"));
        }
        THEN("a command's exception propagates, so the JavaScript Promise rejects") {
            REQUIRE_THROWS_AS(shell.run("fail"), runtime_error);
        }
    }
}

SCENARIO("The terminal shell completes and documents its commands") {
    GIVEN("a shell with sample commands") {
        SampleShell sample;
        auto&       shell = sample.shell;

        THEN("command names complete by prefix, in order") {
            REQUIRE(shell.complete("") == "answer\nfail\ngreet\nlines\nrecord");
            REQUIRE(shell.complete("gr") == "greet");
            REQUIRE(shell.complete("zz").empty());
        }
        THEN("arguments are not completed") {
            REQUIRE(shell.complete("greet A").empty());
        }
        THEN("help lists every command with its aligned description") {
            REQUIRE(shell.help()
                    == "answer  No argument\n"
                       "fail    Throw\n"
                       "greet   Greet someone\n"
                       "lines   Write two lines\n"
                       "record  Record its arguments");
        }
    }
}

SCENARIO("The terminal shell rejects invalid registrations") {
    GIVEN("a shell with sample commands") {
        SampleShell sample;
        auto&       shell = sample.shell;

        THEN("a duplicate or malformed name is rejected") {
            auto name = GENERATE(string{"greet"}, string{""}, string{"two words"}, string{"quo'te"});
            REQUIRE_THROWS_AS(shell.command(name, "rejected", terminal::adaptHandler([] {})), invalid_argument);
        }
        THEN("a command without a handler is rejected") {
            REQUIRE_THROWS_AS(shell.command("ok", "no handler", terminal::Shell::Handler{}), invalid_argument);
            REQUIRE(shell.complete("ok").empty());
        }
    }
}

SCENARIO("Terminal output is capped below the bridge's string limit") {
    GIVEN("a command writing more than the limit, ending in multi-byte characters") {
        terminal::Shell shell;
        shell.command("flood", "Too much output", terminal::adaptHandler([] {
                          string text(terminal::maxOutputBytes - 1, 'x');
                          for (int index = 0; index < 10; ++index)
                              text += "\xc3\xa9";  // U+00E9, two bytes
                          return text;
                      }));

        WHEN("it runs") {
            const auto output = shell.run("flood");
            THEN("the output fits a 16-bit length and ends with a notice") {
                REQUIRE(output.size() < 65536);
                REQUIRE(output.ends_with("[output truncated]\x1b[0m"));
            }
            THEN("it is cut before the first character that would not fit whole") {
                REQUIRE(output.substr(0, terminal::maxOutputBytes - 1) == string(terminal::maxOutputBytes - 1, 'x'));
                REQUIRE(output[terminal::maxOutputBytes - 1] == '\r');
            }
        }
    }
}

SCENARIO("Terminal error messages are capped below the bridge's string limit") {
    GIVEN("a shell and an unknown command name as long as the largest bridge string") {
        terminal::Shell shell;
        const string    name(65535, 'x');

        WHEN("it runs") {
            const auto output = shell.run(name);
            THEN("the diagnostic fits a 16-bit length and is marked as truncated") {
                REQUIRE(output.size() < 65536);
                REQUIRE(contains(output, "[output truncated]"));
            }
            THEN("the diagnostic keeps its colors") {
                REQUIRE(output.starts_with("\x1b[31mUnknown command 'xxx"));
                REQUIRE(output.ends_with("\x1b[0m"));
            }
        }
    }
}

SCENARIO("Terminal registers its bridge functions and built-in commands on the application") {
    GIVEN("a terminal on an application") {
        RecordingApp app;
        auto         terminal = make_unique<Terminal<RecordingApp>>(app, TerminalOptions{.prompt = "demo> ", .banner = "Welcome"});
        terminal->command("greet", "Greet someone", [](CommandArgs args) {
            return "Hello, " + args[0] + "!";
        });

        THEN("the page reads the configured prompt and banner") {
            REQUIRE(app.nullary.at("webfront.terminal.prompt")() == "demo> ");
            REQUIRE(app.nullary.at("webfront.terminal.banner")() == "Welcome");
        }
        THEN("lines run registered commands and complete their names") {
            REQUIRE(app.run("greet Ada") == "Hello, Ada!");
            REQUIRE(app.complete("g") == "greet");
        }
        THEN("help and clear are built in") {
            REQUIRE(contains(app.run("help"), "greet  Greet someone"));
            REQUIRE(contains(app.run("help"), "clear  "));
            REQUIRE(app.run("clear") == Terminal<RecordingApp>::clearScreen);
        }
        THEN("a duplicate command is rejected") {
            REQUIRE_THROWS_AS(terminal->command("help", "again", [] {}), invalid_argument);
        }
        WHEN("the Terminal object is destroyed") {
            terminal.reset();
            THEN("the registered functions still own the commands") {
                REQUIRE(app.run("greet Ada") == "Hello, Ada!");
                REQUIRE(contains(app.run("help"), "greet"));
            }
        }
    }
}

SCENARIO("Terminal commands can continue in the browser") {
    GIVEN("commands returning browser commands in every handler shape") {
        terminal::Shell shell;
        shell.command("fetch", "Continue in the browser", terminal::adaptHandler([](CommandArgs args, ostream& output) {
                          output << "Fetching\n";
                          return BrowserCommand{
                              "/tools/fetch.mjs",
                              {args.begin(), args.end()}
                          };
                      }));
        shell.command("maybe", "Continue only with arguments", terminal::adaptHandler([](CommandArgs args) -> optional<BrowserCommand> {
                          if (args.empty())
                              return nullopt;
                          return BrowserCommand{"/tools/maybe.mjs", {}};
                      }));
        shell.command("plain", "No argument", terminal::adaptHandler([] {
                          return BrowserCommand{"/plain.mjs", {}};
                      }));

        THEN("the result carries the output, the module and the arguments") {
            const auto result = shell.execute("fetch 'a b' c");
            REQUIRE(result.output == "Fetching\r\n");
            REQUIRE(result.browserCommand);
            REQUIRE(result.browserCommand->module == "/tools/fetch.mjs");
            REQUIRE(result.browserCommand->args == vector<string>{"a b", "c"});
            REQUIRE(shell.run("fetch") == "Fetching\r\n");
        }
        THEN("an empty optional continues nowhere") {
            REQUIRE_FALSE(shell.execute("maybe").browserCommand);
            REQUIRE(shell.execute("maybe x").browserCommand->module == "/tools/maybe.mjs");
            REQUIRE(shell.execute("plain").browserCommand->module == "/plain.mjs");
        }
        THEN("ordinary commands and errors continue nowhere") {
            REQUIRE_FALSE(shell.execute("").browserCommand);
            REQUIRE_FALSE(shell.execute("nope").browserCommand);
        }
    }
}

SCENARIO("Terminal browser commands stay on the page's origin") {
    GIVEN("a command continuing in a module chosen by its argument") {
        terminal::Shell shell;
        shell.command("go", "Continue in a module", terminal::adaptHandler([](CommandArgs args) {
                          return BrowserCommand{
                              args[0],
                              {args.begin() + 1, args.end()}
                          };
                      }));

        THEN("an absolute path on the origin is accepted") {
            REQUIRE(shell.execute("go /a/b.mjs?x=1").browserCommand);
        }
        THEN("another origin, a relative path or control characters are rejected") {
            const auto module = GENERATE(as<string>{}, "https://example.com/x.mjs", "//example.com/x.mjs", "x.mjs", "", "/a\\b.mjs", "/a\tb.mjs", "/a\x7f.mjs");
            const auto line   = "go '" + module + "'";
            CAPTURE(module);
            REQUIRE_THROWS_AS(shell.execute(line), invalid_argument);
        }
        THEN("an argument containing a NUL character is rejected") {
            REQUIRE_THROWS_AS(shell.execute(string{"go /a.mjs 'x\0y'", 15}), invalid_argument);
        }
    }
}

SCENARIO("Terminal sends browser commands over the bridge") {
    GIVEN("a terminal with a command continuing in the browser") {
        RecordingApp app;
        Terminal     terminal{app};
        terminal.command("fetch", "Continue in the browser", [](CommandArgs args) {
            return BrowserCommand{
                "/fetch.mjs",
                {args.begin(), args.end()}
            };
        });

        THEN("the arguments cross NUL-terminated, so an empty argument survives") {
            REQUIRE(app.runLine("fetch a '' b")
                    == RecordingApp::LineResult{
                        "",
                        "/fetch.mjs",
                        string{"a\0\0b\0", 5}
            });
            REQUIRE(app.runLine("fetch") == RecordingApp::LineResult{"", "/fetch.mjs", ""});
        }
        THEN("an ordinary command has no module") {
            REQUIRE(app.runLine("clear") == RecordingApp::LineResult{string{Terminal<RecordingApp>::clearScreen}, "", ""});
        }
    }
}
