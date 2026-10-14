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
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront {

/// Arguments of a terminal command, without the command name.
using CommandArgs = std::span<const std::string>;

struct TerminalOptions {
    std::string prompt = "> ";
    std::string banner = "WebFront terminal. Type 'help' to list the commands.";
};

namespace terminal {

/// Splits a command line into words. Whitespace separates words; single quotes keep their content
/// verbatim, double quotes keep it but honor backslash escapes, and an unquoted backslash escapes
/// the next character. Throws std::invalid_argument on an unterminated quote or a trailing backslash.
inline std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> words;
    std::string              word;
    bool                     inWord = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char c = line[index];
        if (c == ' ' || c == '\t') {
            if (inWord)
                words.push_back(std::exchange(word, {}));
            inWord = false;
        } else if (c == '\\') {
            if (++index == line.size())
                throw std::invalid_argument("Trailing backslash");
            word  += line[index];
            inWord = true;
        } else if (c == '\'' || c == '"') {
            inWord = true;
            for (++index;; ++index) {
                if (index == line.size())
                    throw std::invalid_argument("Unterminated quote");
                if (line[index] == c)
                    break;
                if (c == '"' && line[index] == '\\' && index + 1 < line.size())
                    ++index;
                word += line[index];
            }
        } else {
            word  += c;
            inWord = true;
        }
    }
    if (inWord)
        words.push_back(std::move(word));
    return words;
}

/// Bridge strings carry a 16-bit length, so a command output is capped below that limit, on a
/// UTF-8 character boundary, with a visible notice.
inline constexpr std::size_t maxOutputBytes = 60000;

inline std::string capOutput(std::string output) {
    if (output.size() <= maxOutputBytes)
        return output;
    std::size_t cut = maxOutputBytes;
    while (cut > 0 && (static_cast<unsigned char>(output[cut]) & 0xC0) == 0x80)
        --cut;
    output.resize(cut);
    output += "\r\n\x1b[33m[output truncated]\x1b[0m";
    return output;
}

/// Command registry and dispatcher, independent of any connection. Thread-safe: commands may be
/// registered while the server thread runs others.
class Shell {
public:
    using Handler = std::function<void(CommandArgs, std::ostream&)>;

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

    [[nodiscard]] bool contains(std::string_view name) const {
        std::scoped_lock lock{mutex};
        return commands.contains(std::string(name));
    }

    /// Runs one line and returns its output, using "\n" line endings converted to "\r\n" for the
    /// terminal. An empty line produces no output; an unknown command or a malformed line produces
    /// an error message. Exceptions thrown by a command propagate to the caller.
    std::string run(std::string_view line) const {
        std::vector<std::string> words;
        try {
            words = tokenize(line);
        } catch (const std::invalid_argument& error) {
            return errorMessage(error.what());
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
            return errorMessage("Unknown command '" + words.front() + "'. Type 'help' to list the commands.");

        std::ostringstream output;
        (*handler)(CommandArgs{words}.subspan(1), output);
        return capOutput(toTerminalLineEndings(output.str()));
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
        return "\x1b[31m" + capOutput(toTerminalLineEndings(std::string(message))) + "\x1b[0m";
    }

    static std::string toTerminalLineEndings(std::string text) {
        std::string converted;
        converted.reserve(text.size());
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == '\n' && (index == 0 || text[index - 1] != '\r'))
                converted += '\r';
            converted += text[index];
        }
        return converted;
    }
};

/// Adapts the accepted handler shapes to Shell::Handler:
///   (CommandArgs, std::ostream&) -> void, (CommandArgs) -> R, () -> R
/// where R is void or anything that can be written to a std::ostream.
template <typename Function>
Shell::Handler adaptHandler(Function&& function) {
    using Stored     = std::decay_t<Function>;
    auto callable    = std::make_shared<Stored>(std::forward<Function>(function));
    auto writeResult = [](std::ostream& output, auto&& invoke) {
        using R = decltype(invoke());
        if constexpr (std::is_void_v<R>)
            invoke();
        else
            output << invoke();
    };
    if constexpr (std::invocable<Stored&, CommandArgs, std::ostream&>)
        return [callable](CommandArgs args, std::ostream& output) {
            std::invoke(*callable, args, output);
        };
    else if constexpr (std::invocable<Stored&, CommandArgs>)
        return [callable, writeResult](CommandArgs args, std::ostream& output) {
            writeResult(output, [&]() -> decltype(auto) {
                return std::invoke(*callable, args);
            });
        };
    else {
        static_assert(std::invocable<Stored&>,
                      "webfront::Terminal::command: the handler must accept (CommandArgs, std::ostream&), "
                      "(CommandArgs) or no argument.");
        return [callable, writeResult](CommandArgs, std::ostream& output) {
            writeResult(output, [&]() -> decltype(auto) {
                return std::invoke(*callable);
            });
        };
    }
}

}  // namespace terminal

/// Application type serving the terminal page next to the default WebFront assets.
using TerminalApp = BasicWF<NetProvider, fs::DefaultFSWith<fs::TerminalFS>>;

/// Gives an application a command-line interface in the browser.
///
/// The constructor registers the JavaScript-callable functions webfront.terminal.prompt, .banner,
/// .run and .complete on the application, plus a built-in 'help' command. The registered functions
/// own the command registry, so a Terminal object may be destroyed before the application; commands
/// can only be added through a live Terminal. The application's filesystem must serve
/// fs::TerminalFS (TerminalApp does).
template <typename App>
class Terminal {
public:
    static constexpr std::string_view page = fs::TerminalFS::page;
    /// Erases the screen and the scrollback and homes the cursor. The page writes no line break
    /// after an output ending with the cursor-home sequence.
    static constexpr std::string_view clearScreen = "\x1b[2J\x1b[3J\x1b[H";

    explicit Terminal(App& app, TerminalOptions options = {}) : shell(std::make_shared<terminal::Shell>()) {
        shell->command("help", "List the available commands", [weakShell = std::weak_ptr(shell)](CommandArgs, std::ostream& output) {
            if (auto registry = weakShell.lock())
                output << registry->help();
        });
        shell->command("clear", "Clear the screen and its scrollback", [](CommandArgs, std::ostream& output) {
            output << clearScreen;
        });
        app.template cppFunction<std::string>("webfront.terminal.prompt", [prompt = options.prompt] {
            return prompt;
        });
        app.template cppFunction<std::string>("webfront.terminal.banner", [banner = options.banner] {
            return banner;
        });
        app.template cppFunction<std::string, std::string>("webfront.terminal.run", [registry = shell](const std::string& line) {
            return registry->run(line);
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
