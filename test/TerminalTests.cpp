#include <catch2/catch_test_macros.hpp>
#include <components/Terminal.hpp>

#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using namespace std;
using namespace webfront;

namespace {

// Records the functions a Terminal registers, like BasicWF::cppFunction, and calls them directly.
struct RecordingApp {
    map<string, function<string()>>       nullary;
    map<string, function<string(string)>> unary;

    template <typename R, typename... Args>
    void cppFunction(string name, auto&& function) {
        static_assert(is_same_v<R, string>);
        if constexpr (sizeof...(Args) == 0)
            nullary.try_emplace(std::move(name), std::forward<decltype(function)>(function));
        else
            unary.try_emplace(std::move(name), std::forward<decltype(function)>(function));
    }

    string run(const string& line) {
        return unary.at("webfront.terminal.run")(line);
    }
    string complete(const string& line) {
        return unary.at("webfront.terminal.complete")(line);
    }
};

bool contains(const string& text, const string& part) {
    return text.find(part) != string::npos;
}

}  // namespace

SCENARIO("Terminal lines are split into words") {
    GIVEN("lines with whitespace, quotes and escapes") {
        THEN("whitespace separates words and runs of it are ignored") {
            REQUIRE(terminal::tokenize("  greet \t Ada  ") == vector<string>{"greet", "Ada"});
            REQUIRE(terminal::tokenize("").empty());
            REQUIRE(terminal::tokenize("   ").empty());
        }
        THEN("quotes group words, and empty quotes give an empty word") {
            REQUIRE(terminal::tokenize(R"(say "hello world" 'a b')") == vector<string>{"say", "hello world", "a b"});
            REQUIRE(terminal::tokenize(R"(x "" y)") == vector<string>{"x", "", "y"});
            REQUIRE(terminal::tokenize(R"(pre"fix"post)") == vector<string>{"prefixpost"});
        }
        THEN("backslashes escape outside quotes and inside double quotes, not inside single quotes") {
            REQUIRE(terminal::tokenize(R"(a\ b)") == vector<string>{"a b"});
            REQUIRE(terminal::tokenize(R"("say \"hi\"")") == vector<string>{R"(say "hi")"});
            REQUIRE(terminal::tokenize(R"('a\b')") == vector<string>{R"(a\b)"});
        }
        THEN("an unterminated quote or a trailing backslash is rejected") {
            REQUIRE_THROWS_AS(terminal::tokenize(R"(say "hello)"), invalid_argument);
            REQUIRE_THROWS_AS(terminal::tokenize("say 'hello"), invalid_argument);
            REQUIRE_THROWS_AS(terminal::tokenize(R"(say \)"), invalid_argument);
        }
    }
}

SCENARIO("The terminal shell dispatches lines to registered commands") {
    GIVEN("a shell with commands of every accepted handler shape") {
        terminal::Shell shell;
        vector<string>  received;
        shell.command("greet", "Greet someone", terminal::adaptHandler([](CommandArgs args) {
                          return "Hello, " + (args.empty() ? string{"world"} : args[0]) + "!";
                      }));
        shell.command("record", "Record its arguments", terminal::adaptHandler([&received](CommandArgs args) {
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

        THEN("the command receives its arguments without its name") {
            REQUIRE(shell.run("greet Ada") == "Hello, Ada!");
            REQUIRE(shell.run("greet") == "Hello, world!");
            REQUIRE(shell.run("record 'a b' c").empty());
            REQUIRE(received == vector<string>{"a b", "c"});
            REQUIRE(shell.run("answer ignored") == "42");
        }
        THEN("written line breaks become terminal line breaks") {
            REQUIRE(shell.run("lines") == "one\r\ntwo");
        }
        THEN("an empty line produces no output") {
            REQUIRE(shell.run("").empty());
            REQUIRE(shell.run("   ").empty());
        }
        THEN("an unknown command or a malformed line produces an error message, not an exception") {
            const auto unknown = shell.run("nope");
            REQUIRE(contains(unknown, "Unknown command 'nope'"));
            REQUIRE(unknown.starts_with("\x1b[31m"));
            REQUIRE(contains(shell.run(R"(greet "Ada)"), "Unterminated quote"));
        }
        THEN("a command's exception propagates, so the JavaScript Promise rejects") {
            REQUIRE_THROWS_AS(shell.run("fail"), runtime_error);
        }
        THEN("command names complete by prefix, in order, and arguments are not completed") {
            REQUIRE(shell.complete("") == "answer\nfail\ngreet\nlines\nrecord");
            REQUIRE(shell.complete("gr") == "greet");
            REQUIRE(shell.complete("zz").empty());
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
        THEN("a duplicate or malformed name is rejected") {
            auto handler = terminal::adaptHandler([] {});
            REQUIRE_THROWS_AS(shell.command("greet", "again", handler), invalid_argument);
            REQUIRE_THROWS_AS(shell.command("", "empty", handler), invalid_argument);
            REQUIRE_THROWS_AS(shell.command("two words", "space", handler), invalid_argument);
            REQUIRE_THROWS_AS(shell.command("ok", "no handler", terminal::Shell::Handler{}), invalid_argument);
            REQUIRE_FALSE(shell.contains("ok"));
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
            THEN("the output is truncated on a character boundary with a notice") {
                REQUIRE(output.size() < 65536);
                REQUIRE(output.ends_with("[output truncated]\x1b[0m"));
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
            THEN("the diagnostic fits a 16-bit length, keeps its colors and is marked as truncated") {
                REQUIRE(output.size() < 65536);
                REQUIRE(output.starts_with("\x1b[31mUnknown command 'xxx"));
                REQUIRE(contains(output, "[output truncated]"));
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
