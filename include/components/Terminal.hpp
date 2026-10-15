/// @brief Interactive terminal component: an xterm.js page served from embedded assets, whose line
/// editor sends each validated line to commands registered in C++.
#pragma once

#include "../system/DefaultFS.hpp"
#include "../system/TerminalFS.hpp"
#include "../WebFront.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront {

/// Arguments of a terminal command, without the command name.
using CommandArgs = std::span<const std::string>;

/// A command that continues in the browser, for work the browser does better, such as HTTPS
/// downloads. Once the command's output is shown, the page imports `module`, an absolute path on the
/// application's origin such as "/webfront/components/tools/wf.mjs", and awaits its exported
/// run(args, io); see docs/UI_COMPONENTS.md.
struct BrowserCommand {
    std::string              module;
    std::vector<std::string> args;
};

struct TerminalOptions {
    std::string prompt = "> ";
    std::string banner = "WebFront terminal. Type 'help' to list the commands.";
};

namespace terminal {

namespace detail {

/// Shell-style word splitting, one character at a time; see tokenize().
class LineSplitter {
public:
    explicit LineSplitter(std::string_view text) : line(text) {}

    std::vector<std::string> split() {
        while (index < line.size())
            take(line[index++]);
        endWord();
        return std::move(words);
    }

private:
    std::string_view         line;
    std::size_t              index = 0;
    std::vector<std::string> words;
    std::string              word;
    bool                     inWord = false;

    void take(char c) {
        if (c == ' ' || c == '\t')
            return endWord();
        inWord = true;
        if (c == '\\')
            readEscaped();
        else if (c == '\'' || c == '"')
            readQuoted(c);
        else
            word += c;
    }

    void readEscaped() {
        if (index == line.size())
            throw std::invalid_argument("Trailing backslash");
        word += line[index++];
    }

    char nextQuoted() {
        if (index == line.size())
            throw std::invalid_argument("Unterminated quote");
        return line[index++];
    }

    void readQuoted(char quote) {
        for (char c = nextQuoted(); c != quote; c = nextQuoted())
            word += quote == '"' && c == '\\' ? escapedInDoubleQuotes() : c;
    }

    // A backslash ending the line stays literal; the missing closing quote is reported next.
    char escapedInDoubleQuotes() {
        return index < line.size() ? line[index++] : '\\';
    }

    void endWord() {
        if (inWord)
            words.push_back(std::exchange(word, {}));
        inWord = false;
    }
};

}  // namespace detail

/// Splits a command line into words. Whitespace separates words; single quotes keep their content
/// verbatim, double quotes keep it but honor backslash escapes, and an unquoted backslash escapes
/// the next character. Throws std::invalid_argument on an unterminated quote or a trailing backslash.
inline std::vector<std::string> tokenize(std::string_view line) {
    return detail::LineSplitter{line}.split();
}

/// Bridge strings carry a 16-bit length, so a command output is capped below that limit, on a
/// UTF-8 character boundary, with a visible notice.
inline constexpr std::size_t maxOutputBytes = 60000;

namespace detail {

/// Prepares text for the terminal: "\n" becomes "\r\n", then the result is capped to
/// maxOutputBytes.
inline std::string toTerminalText(std::string_view text) {
    std::string converted;
    converted.reserve(text.size());
    char previous = '\0';
    for (const char c : text) {
        if (c == '\n' && previous != '\r')
            converted += '\r';
        converted += c;
        previous   = c;
    }
    if (converted.size() <= maxOutputBytes)
        return converted;
    std::size_t cut = maxOutputBytes;
    while (cut > 0 && (static_cast<unsigned char>(converted[cut]) & 0xC0) == 0x80)
        --cut;
    converted.resize(cut);
    return converted + "\r\n\x1b[33m[output truncated]\x1b[0m";
}

}  // namespace detail

/// Command registry and dispatcher, independent of any connection. Thread-safe: commands may be
/// registered while the server thread runs others.
class Shell {
public:
    using Handler = std::function<std::optional<BrowserCommand>(CommandArgs, std::ostream&)>;

    /// What a line produces: the output to show, then the browser command continuing it, if any.
    struct Result {
        std::string                   output;
        std::optional<BrowserCommand> browserCommand;
    };

    /// Registers a command. Throws std::invalid_argument if the name is empty, contains whitespace
    /// or quotes, or is already registered.
    void command(std::string name, std::string help, Handler handler) {
        if (name.empty() || name.find_first_of(" \t'\"\\") != std::string::npos)
            throw std::invalid_argument("Invalid terminal command name '" + name + "'");
        if (!handler)
            throw std::invalid_argument("Terminal command '" + name + "' has no handler");
        std::scoped_lock lock{mutex};
        if (!commands.try_emplace(name, Command{std::move(help), std::make_shared<Handler>(std::move(handler))}).second)
            throw std::invalid_argument("Terminal command '" + name + "' is already registered");
    }

    /// Runs one line. The output uses "\n" line endings converted to "\r\n" for the terminal. An
    /// empty line produces no output; an unknown command or a malformed line produces an error
    /// message. Exceptions thrown by a command propagate to the caller, and a browser command whose
    /// module is not an absolute path on the page's origin, or whose arguments contain a NUL
    /// character, throws std::invalid_argument.
    Result execute(std::string_view line) const {
        std::vector<std::string> words;
        try {
            words = tokenize(line);
        } catch (const std::invalid_argument& error) {
            return {errorMessage(error.what()), {}};
        }
        if (words.empty())
            return {};

        std::shared_ptr<Handler> handler;
        {
            std::scoped_lock lock{mutex};
            if (auto found = commands.find(words.front()); found != commands.end())
                handler = found->second.handler;
        }
        if (!handler)
            return {errorMessage("Unknown command '" + words.front() + "'. Type 'help' to list the commands."), {}};

        std::ostringstream output;
        auto               browserCommand = (*handler)(CommandArgs{words}.subspan(1), output);
        if (browserCommand)
            validate(*browserCommand);
        return {detail::toTerminalText(output.view()), std::move(browserCommand)};
    }

    /// The output of execute(line), for commands that do not continue in the browser.
    std::string run(std::string_view line) const {
        return execute(line).output;
    }

    /// Completes the command name of a line: returns the matching names separated by "\n", or an
    /// empty string once the line already contains a space (arguments are not completed).
    [[nodiscard]] std::string complete(std::string_view line) const {
        if (line.find_first_of(" \t") != std::string_view::npos)
            return {};
        std::string      candidates;
        std::scoped_lock lock{mutex};
        for (auto found = commands.lower_bound(std::string(line)); found != commands.end() && found->first.starts_with(line); ++found) {
            if (!candidates.empty())
                candidates += '\n';
            candidates += found->first;
        }
        return candidates;
    }

    /// Lists every command with its help text, aligned, one per line.
    [[nodiscard]] std::string help() const {
        std::scoped_lock lock{mutex};
        std::size_t      width = 0;
        for (const auto& [name, command] : commands)
            width = std::max(width, name.size());
        std::string text;
        for (const auto& [name, command] : commands) {
            if (!text.empty())
                text += '\n';
            text += name + std::string(width - name.size() + 2, ' ') + command.help;
        }
        return text;
    }

private:
    struct Command {
        std::string              help;
        std::shared_ptr<Handler> handler;
    };

    mutable std::mutex                          mutex;
    std::map<std::string, Command, std::less<>> commands;

    // Messages may quote the user's line, so they are capped like command output, before the
    // color codes are added so the closing reset always survives.
    static std::string errorMessage(std::string_view message) {
        return "\x1b[31m" + detail::toTerminalText(message) + "\x1b[0m";
    }

    // The page passes the module to import(), so it must not name another origin; the arguments
    // cross the bridge NUL-terminated.
    static void validate(const BrowserCommand& command) {
        if (!isPathOnOrigin(command.module))
            throw std::invalid_argument("Invalid browser command module '" + command.module + "'");
        if (std::ranges::any_of(command.args, [](const std::string& arg) {
                return arg.contains('\0');
            }))
            throw std::invalid_argument("A browser command argument contains a NUL character");
    }

    static bool isPathOnOrigin(std::string_view module) {
        if (!module.starts_with('/') || module.starts_with("//"))
            return false;
        return std::ranges::none_of(module, [](char c) {
            return static_cast<unsigned char>(c) < 0x20 || std::string_view("\\\x7f").contains(c);
        });
    }
};

/// Adapts the accepted handler shapes to Shell::Handler:
///   (CommandArgs, std::ostream&) -> void, (CommandArgs) -> R, () -> R
/// where R is void or anything that can be written to a std::ostream. Any shape may instead return
/// a BrowserCommand or a std::optional<BrowserCommand> to continue in the browser.
template <typename Function>
Shell::Handler adaptHandler(Function&& function) {
    using Stored     = std::decay_t<Function>;
    auto callable    = std::make_shared<Stored>(std::forward<Function>(function));
    auto writeResult = [](std::ostream& output, auto&& invoke) -> std::optional<BrowserCommand> {
        using R = decltype(invoke());
        if constexpr (std::is_void_v<R>)
            invoke();
        else if constexpr (std::is_convertible_v<R, std::optional<BrowserCommand>>)
            return invoke();
        else
            output << invoke();
        return std::nullopt;
    };
    if constexpr (std::invocable<Stored&, CommandArgs, std::ostream&>)
        return [callable, writeResult](CommandArgs args, std::ostream& output) {
            return writeResult(output, [&]() -> decltype(auto) {
                return std::invoke(*callable, args, output);
            });
        };
    else if constexpr (std::invocable<Stored&, CommandArgs>)
        return [callable, writeResult](CommandArgs args, std::ostream& output) {
            return writeResult(output, [&]() -> decltype(auto) {
                return std::invoke(*callable, args);
            });
        };
    else {
        static_assert(std::invocable<Stored&>,
                      "webfront::Terminal::command: the handler must accept (CommandArgs, std::ostream&), "
                      "(CommandArgs) or no argument.");
        return [callable, writeResult](CommandArgs, std::ostream& output) {
            return writeResult(output, [&]() -> decltype(auto) {
                return std::invoke(*callable);
            });
        };
    }
}

namespace detail {

/// The bridge form of a line's result: the output, the browser command's module ("" for none) and
/// its arguments, each followed by a NUL character.
inline std::tuple<std::string, std::string, std::string> toBridge(Shell::Result result) {
    if (!result.browserCommand)
        return {std::move(result.output), {}, {}};
    std::string args;
    for (const auto& arg : result.browserCommand->args)
        args += arg + '\0';
    return {std::move(result.output), std::move(result.browserCommand->module), std::move(args)};
}

}  // namespace detail

}  // namespace terminal

/// Application type serving the terminal page next to the default WebFront assets.
using TerminalApp = BasicWF<NetProvider, fs::DefaultFSWith<fs::TerminalFS>>;

/// Gives an application a command-line interface in the browser.
///
/// The constructor registers the JavaScript-callable functions webfront.terminal.prompt, .banner,
/// .run (returning [output, browser module, NUL-terminated arguments]) and .complete on the
/// application, plus built-in 'help' and 'clear' commands. The registered functions own the command
/// registry, so a Terminal object may be destroyed before the application; commands can only be
/// added through a live Terminal. The application's filesystem must serve fs::TerminalFS
/// (TerminalApp does).
template <typename App>
class Terminal {
public:
    static constexpr std::string_view page = fs::TerminalFS::page;
    /// Erases the screen and the scrollback and homes the cursor. The page writes no line break
    /// after an output ending with the cursor-home sequence.
    static constexpr std::string_view clearScreen = "\x1b[2J\x1b[3J\x1b[H";

    explicit Terminal(App& app, TerminalOptions options = {}) : shell(std::make_shared<terminal::Shell>()) {
        command("help", "List the available commands", [weakShell = std::weak_ptr(shell)](CommandArgs, std::ostream& output) {
            if (auto registry = weakShell.lock())
                output << registry->help();
        });
        command("clear", "Clear the screen and its scrollback", [](CommandArgs, std::ostream& output) {
            output << clearScreen;
        });
        app.template cppFunction<std::string>("webfront.terminal.prompt", [prompt = options.prompt] {
            return prompt;
        });
        app.template cppFunction<std::string>("webfront.terminal.banner", [banner = options.banner] {
            return banner;
        });
        app.template cppFunction<std::tuple<std::string, std::string, std::string>, std::string>("webfront.terminal.run",
                                                                                                 [registry = shell](const std::string& line) {
                                                                                                     return terminal::detail::toBridge(registry->execute(line));
                                                                                                 });
        app.template cppFunction<std::string, std::string>("webfront.terminal.complete", [registry = shell](const std::string& line) {
            return registry->complete(line);
        });
    }

    /// Registers a command; see terminal::adaptHandler for the accepted handler shapes and
    /// terminal::Shell::command for the errors.
    template <typename Function>
    Terminal& command(std::string name, std::string help, Function&& handler) {
        shell->command(std::move(name), std::move(help), terminal::adaptHandler(std::forward<Function>(handler)));
        return *this;
    }

private:
    std::shared_ptr<terminal::Shell> shell;
};

template <typename App>
Terminal(App&, TerminalOptions = {}) -> Terminal<App>;

}  // namespace webfront
