/// @brief Terminal commands of an application: coroutines that the developer environment's terminal discovers and
/// runs, one browser session at a time, streaming their output and cancelled by Ctrl+C, a disconnection or shutdown.
#pragma once

#include "networking/Tasks.hpp"
#include "Task.hpp"
#include "UISubscribers.hpp"
#include "utils/Json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront {

namespace commands {

/// Bridge strings carry a 16-bit length: output is sent in pieces of at most this many bytes.
inline constexpr std::size_t maxChunkBytes = 60000;

/// Exit code of a command stopped by Ctrl+C, a disconnection or the application's shutdown.
inline constexpr int interruptedExitCode = 130;

/// Names the terminal itself handles, which an application cannot register.
inline constexpr std::array<std::string_view, 5> reservedNames{"wf", "clear", "exit", "help", "history"};

namespace detail {

namespace ex = networking::ex;

/// Splits text into pieces of at most maxBytes (at least 4), never inside a UTF-8 character. Bytes that are not
/// UTF-8, with no character start in a piece, are cut at maxBytes.
inline std::vector<std::string_view> chunks(std::string_view text, std::size_t maxBytes = maxChunkBytes) {
    std::vector<std::string_view> pieces;
    while (text.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        if (cut == 0)
            cut = maxBytes;
        pieces.push_back(text.substr(0, cut));
        text.remove_prefix(cut);
    }
    if (!text.empty())
        pieces.push_back(text);
    return pieces;
}

/// The command's arguments, a JSON array of strings sent by the terminal.
inline std::vector<std::string> parseArguments(std::string_view json) {
    std::vector<std::string> arguments;
    json::Reader             reader{json};
    reader.array([&] {
        arguments.push_back(reader.string());
    });
    reader.end();
    return arguments;
}

inline void validateName(std::string_view name) {
    const auto invalid = name.empty() || std::ranges::any_of(name, [](char c) {
        return static_cast<unsigned char>(c) <= ' ' || c == '\x7f' || c == '\'' || c == '"' || c == '\\';
    });
    if (invalid)
        throw std::invalid_argument("Invalid command name '" + std::string(name) + "'");
    if (std::ranges::find(reservedNames, name) != reservedNames.end())
        throw std::invalid_argument("Command name '" + std::string(name) + "' is reserved by the terminal");
}

/// One running command, shared by its coroutine, its Context and the registry.
template <typename UI>
struct Run {
    UI                       ui;
    std::uint32_t            id;
    std::vector<std::string> arguments;
    int                      exitCode = 0;
    ex::inplace_stop_token   stop{};
    std::atomic<bool>        disconnected{false};

    Run(UI caller, std::uint32_t commandId, std::vector<std::string> args) : ui(std::move(caller)), id(commandId), arguments(std::move(args)) { }

    /// Calls a browser function of the terminal, unless the browser is gone. Returns false when the call could not
    /// be made.
    bool send(std::string_view function, auto&&... values) {
        if (disconnected.load())
            return false;
        try {
            ui.jsFunction(function)(id, std::forward<decltype(values)>(values)...);
            return true;
        } catch (const std::exception&) {
            disconnected = true;  // the link is gone: nothing more reaches this browser
            return false;
        }
    }
};

}  // namespace detail

/// What a command receives: its arguments, its output and its cancellation. Cheap to copy; valid while the command
/// runs. Its members are called from the command's coroutine, on the server thread.
template <typename UI>
class Context {
public:
    explicit Context(std::shared_ptr<detail::Run<UI>> command) : run(std::move(command)) { }

    /// The words after the command name, split like a shell line by the terminal.
    [[nodiscard]] const std::vector<std::string>& args() const noexcept {
        return run->arguments;
    }

    /// The browser running the command, for jsFunctionSender calls or resumeAfter.
    [[nodiscard]] UI ui() const {
        return run->ui;
    }

    /// Streams text to the terminal at once, in pieces under the bridge's string limit. "\n" ends a line. Nothing is
    /// sent once the command is stopped, since the terminal no longer waits for it.
    void write(std::string_view text) const {
        if (run->stop.stop_requested())
            return;
        for (const auto piece : detail::chunks(text))
            if (!run->send("webfront.commands.output", piece))
                return;
    }

    /// Sets the exit code reported when the command returns; 0 by default. An exception reports 1.
    void exit(int code) const noexcept {
        run->exitCode = code;
    }

    /// True once Ctrl+C, a disconnection or the application's shutdown asked the command to stop. Awaited senders
    /// observe it themselves; loops that do not await check it.
    [[nodiscard]] bool stopRequested() const noexcept {
        return run->stop.stop_requested();
    }

    [[nodiscard]] networking::ex::inplace_stop_token stopToken() const noexcept {
        return run->stop;
    }

private:
    std::shared_ptr<detail::Run<UI>> run;
};

namespace detail {

/// The registered commands and the commands running in each connected browser. Thread-safe.
template <typename UI>
class Registry : public std::enable_shared_from_this<Registry<UI>> {
public:
    using Handler = std::function<Task(Context<UI>)>;

    void add(std::string name, std::string help, Handler handler) {
        validateName(name);
        if (!handler)
            throw std::invalid_argument("Command '" + name + "' has no handler");
        std::scoped_lock lock{mutex};
        if (!commands.try_emplace(name, Command{std::move(help), std::make_shared<Handler>(std::move(handler))}).second)
            throw std::invalid_argument("Command '" + name + "' is already registered");
    }

    /// The commands as a JSON array of {"name", "help"} objects, sorted by name.
    [[nodiscard]] std::string list() const {
        std::scoped_lock lock{mutex};
        std::string      json = "[";
        for (const auto& [name, command] : commands) {
            if (json.size() > 1)
                json += ',';
            json += "{\"name\":" + json::quote(name) + ",\"help\":" + json::quote(command.help) + '}';
        }
        return json + ']';
    }

    /// Spawns a command for the browser, which numbers its commands. Throws, rejecting the browser's call, for an
    /// unknown command, malformed arguments, a number already running or an application shutting down.
    void start(UI ui, std::uint32_t id, const std::string& name, const std::string& argumentsJson) {
        auto                     arguments = parseArguments(argumentsJson);
        std::shared_ptr<Handler> handler;
        const Key                key{ui.id(), id};
        auto                     run = std::make_shared<Run<UI>>(ui, id, std::move(arguments));
        {
            std::scoped_lock lock{mutex};
            const auto       found = commands.find(name);
            if (found == commands.end())
                throw std::invalid_argument("Unknown command '" + name + "'");
            if (running.contains(key))
                throw std::invalid_argument("Command " + std::to_string(id) + " is already running");
            handler = found->second.handler;
            running.try_emplace(key, Running{run});
        }
        auto handle = ui.spawn(execute(this->weak_from_this(), std::move(handler), run));
        if (handle.stopRequested()) {  // refused, or accepted only to be cancelled by the shutdown
            finished(key, run);
            throw std::runtime_error("The application is shutting down");
        }
        std::scoped_lock lock{mutex};
        if (auto found = running.find(key); found != running.end() && found->second.run == run) {
            found->second.handle = std::move(handle);
            if (found->second.cancelRequested)
                found->second.handle->requestStop();
        }
    }

    /// Ctrl+C: asks the command to stop. The browser no longer waits for it; a command that already ended is ignored.
    void cancel(const UI& ui, std::uint32_t id) {
        std::optional<TaskHandle> handle;
        {
            std::scoped_lock lock{mutex};
            const auto       found = running.find(Key{ui.id(), id});
            if (found == running.end())
                return;
            found->second.cancelRequested = true;
            handle                        = found->second.handle;
        }
        if (handle)
            handle->requestStop();  // outside the lock: the command may complete inline and unregister itself
    }

    /// A disconnected browser: its commands are stopped and never write to the connection again.
    void closed(const UI& ui) {
        std::vector<TaskHandle> handles;
        {
            std::scoped_lock lock{mutex};
            for (auto& [key, command] : running) {
                if (key.first != ui.id())
                    continue;
                command.run->disconnected = true;
                command.cancelRequested   = true;
                if (command.handle)
                    handles.push_back(*command.handle);
            }
        }
        for (const auto& handle : handles)
            handle.requestStop();
    }

    void keep(UISubscription subscription) {
        closedSubscription = std::move(subscription);
    }

    [[nodiscard]] std::size_t runningCount() const {
        std::scoped_lock lock{mutex};
        return running.size();
    }

private:
    using Key = std::pair<std::remove_cvref_t<decltype(std::declval<const UI&>().id())>, std::uint32_t>;

    struct Command {
        std::string              help;
        std::shared_ptr<Handler> handler;
    };
    struct Running {
        std::shared_ptr<Run<UI>>  run;
        std::optional<TaskHandle> handle{};
        bool                      cancelRequested = false;
    };

    mutable std::mutex                          mutex;
    std::map<std::string, Command, std::less<>> commands;
    std::map<Key, Running>                      running;
    UISubscription                              closedSubscription;

    void finished(const Key& key, const std::shared_ptr<Run<UI>>& run) {
        std::scoped_lock lock{mutex};
        if (auto found = running.find(key); found != running.end() && found->second.run == run)
            running.erase(found);
    }

    // Runs the command, then reports its exit code and error message to the browser, which ignores the report of a
    // command it stopped waiting for. A stopped command still reports: its coroutine was unwound, not this one.
    static Task execute(std::weak_ptr<Registry> registry, std::shared_ptr<Handler> handler, std::shared_ptr<Run<UI>> run) {
        run->stop    = co_await ex::read_env(ex::get_stop_token);
        bool stopped = false;
        std::string error;
        try {
            co_await ex::let_stopped((*handler)(Context<UI>{run}), [&stopped]() noexcept {
                stopped = true;
                return ex::just();
            });
        } catch (const std::exception& exception) {
            error = *exception.what() ? exception.what() : "C++ exception";
        } catch (...) {
            error = "Unknown C++ exception";
        }
        if (auto owner = registry.lock())
            owner->finished(Key{run->ui.id(), run->id}, run);
        const int code = stopped ? interruptedExitCode : !error.empty() ? 1 : run->exitCode;
        run->send("webfront.commands.exit", code, error.empty() ? std::string_view{} : chunks(error).front());
    }
};

}  // namespace detail
}  // namespace commands

/// Adds an application's commands to the terminal of the developer environment (BasicDeveloperApp::bootstrap).
///
/// The constructor registers the bridge functions webfront.commands.list, .start and .cancel, through which each
/// connected browser discovers the commands and runs them in its own session. A command is a coroutine,
/// `webfront::Task(webfront::commands::Context<UI>)`, spawned on the server thread like ui.spawn:
///   - it streams its output with Context::write, never through one blocking reply;
///   - Ctrl+C in the terminal, the browser's disconnection or the application's shutdown requests its stop, which the
///     senders it awaits observe (ui.resumeAfter, ui.jsFunctionSender, ...). The terminal does not wait for a stopped
///     command: its late output and exit report are ignored, and the next command can start at once;
///   - an exception it throws is shown as an error in the terminal, with exit code 1.
/// A command must not block the server thread, which serves every browser: synchronous handlers are refused at
/// compile time, and long computations belong on another thread. Create one Commands object per application; the
/// registered bridge functions own the commands, so the object may be destroyed before the application.
template <typename App>
class Commands {
public:
    using UI      = typename App::UI;
    using Context = commands::Context<UI>;

    explicit Commands(App& app) : registry(std::make_shared<commands::detail::Registry<UI>>()) {
        registry->keep(app.subscribeUIClosed([weak = std::weak_ptr(registry)](UI ui) {
            if (auto owner = weak.lock())
                owner->closed(ui);
        }));
        app.template cppFunction<std::string>("webfront.commands.list", [owner = registry] {
            return owner->list();
        });
        app.template cppFunction<void, UI, std::uint32_t, std::string, std::string>(
            "webfront.commands.start", [owner = registry](UI ui, std::uint32_t id, const std::string& name, const std::string& arguments) {
                owner->start(std::move(ui), id, name, arguments);
            });
        app.template cppFunction<void, UI, std::uint32_t>("webfront.commands.cancel", [owner = registry](UI ui, std::uint32_t id) {
            owner->cancel(ui, id);
        });
    }

    /// Registers a command. Throws std::invalid_argument for an empty name, one with whitespace, quotes or
    /// backslashes, a name the terminal reserves (commands::reservedNames) or one already registered.
    template <typename Function>
    Commands& add(std::string name, std::string help, Function&& handler) {
        static_assert(std::is_invocable_r_v<Task, std::decay_t<Function>&, Context>,
                      "webfront::Commands::add: a command is a coroutine returning webfront::Task and taking a "
                      "webfront::commands::Context; a synchronous handler would block the server thread.");
        auto callable = std::make_shared<std::decay_t<Function>>(std::forward<Function>(handler));
        registry->add(std::move(name), std::move(help), [callable](Context context) -> Task {
            return std::invoke(*callable, std::move(context));
        });
        return *this;
    }

    /// The commands running in all browsers, for tests and diagnostics.
    [[nodiscard]] std::size_t running() const {
        return registry->runningCount();
    }

private:
    std::shared_ptr<commands::detail::Registry<UI>> registry;
};

template <typename App>
Commands(App&) -> Commands<App>;

}  // namespace webfront
