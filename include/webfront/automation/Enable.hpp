// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-08
//
// WebFront — automation
// Role: Opt-in automation of a consumer's own developer window: enableAutomation mounts the controller, the page binding and the MCP listener.
// Thread: bridge-io | Alloc: unrestricted
// LLM: public entry point (ADR-025); nothing is registered unless called, grants are fixed by the native owner

// Opt-in automation of a consumer's own developer window.
//
// `enableAutomation(app, options)` mounts the controller (`Session`) and the page binding (`Binding`) on an application
// that already called `bootstrap("developer")`, then serves MCP from this process, over Streamable HTTP on a loopback
// listener of its own (ADR-025), and publishes a discovery record. An MCP client connects to `mcpUrl()` with the private
// configuration `clientConfiguration()`; no other program is involved. Check, build, test, preview, inspect and interact
// run in the page of the application, whose `cppFunction` callbacks stay registered. Nothing is registered unless this
// function is called, and the grants are fixed here by the native owner. Automation does not exist in a distribution
// build: this header refuses to compile with `WEBFRONT_DISTRIBUTION`.
//
// ```cpp
// webfront::NativeApp app{project};
// app.bootstrap("developer");
// app.cppFunction("bf.render", render);
// auto automation = webfront::automation::enableAutomation(app, {.grants = {"check", "build", "inspect", "interact"}});
// app.show(automation->launchPage());
// automation->close();
// app.shutdown();
// ```

#pragma once
#if defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION
    #error "WebFront automation does not exist in a distribution build (WEBFRONT_DISTRIBUTION, ADR-025): gate enableAutomation out of it"
#endif
#include "Binding.hpp"
#include "Channel.hpp"
#include "Discovery.hpp"
#include "McpHttp.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

namespace webfront::automation {
/// `from + after`, saturated: a very large duration (`seconds::max()` to disable a delay) must not overflow the clock's tick
/// count, which is undefined behavior and would yield a deadline already past. Nothing is computed that can overflow: the room
/// left before `time_point::max()` is measured in whole seconds (never more than twice the clock's range, whatever the origin,
/// negative included), `after` is compared to it without being incremented, and the sum is made in two steps when `after`
/// alone exceeds the clock's tick range while the deadline stays representable. A zero or negative duration returns `from`.
template <typename Duration>
[[nodiscard]] constexpr std::chrono::steady_clock::time_point deadlineAfter(std::chrono::steady_clock::time_point from, Duration after) noexcept {
    using Clock = std::chrono::steady_clock;
    using std::chrono::duration_cast;
    using std::chrono::seconds;
    if (after <= Duration::zero())
        return from;
    const auto capacity = duration_cast<seconds>(Clock::time_point::max().time_since_epoch());
    const auto room     = capacity - duration_cast<seconds>(from.time_since_epoch());
    const auto whole    = duration_cast<seconds>(after);
    // One second of margin for the truncations; `room - 1` cannot overflow, `room` being at least 1.
    if (whole >= room - seconds{1})
        return Clock::time_point::max();
    if (whole < capacity)
        return from + after;  // `after` converts to ticks: it is below the clock's range
    // `after` exceeds the range of the ticks but the deadline does not: `from` is negative, so a first step of the largest
    // whole number of `Duration` the clock can hold stays representable, then the rest.
    const auto step = duration_cast<Duration>(Clock::duration::max());
    return from + duration_cast<Clock::duration>(step) + (after - step);
}

/// The end of the startup window, saturated (see `deadlineAfter`).
[[nodiscard]] constexpr std::chrono::steady_clock::time_point startupDeadline(std::chrono::steady_clock::time_point launched,
                                                                              std::chrono::seconds                  startup) noexcept {
    return deadlineAfter(launched, startup);
}

struct AutomationOptions {
    /// Granted operations (`check`, `build`, `test`, `preview`, `inspect`, `interact`, and the setup grants). None
    /// leaves only session discovery.
    std::vector<std::string> grants{};
    /// The application callbacks the `call` grant may invoke (`callbacks = {"bf.render"}`): an explicit list of names, never a
    /// pattern. The agent sees the names, gives them one string at most and reads a scalar or list back; `webfront.*` names are
    /// refused. Needs the `call` grant; without it nothing of the application can be called.
    std::vector<std::string> callbacks{};
    /// The port of the MCP listener: 0 (the default) lets the OS choose; a chosen port is never replaced, its being taken
    /// makes the activation throw.
    unsigned short mcpPort = 0;
    /// Optional durable Bearer token, supplied by the application; absent selects a random token per session.
    std::optional<std::string> mcpToken{};
    /// Explicitly allow local MCP clients without a token. Development only.
    bool disableMcpAuthentication = false;
    /// A new private directory outside the project that receives the client configuration (`mcp.json`). Empty (the
    /// default), a directory named after the session is created in the discovery directory.
    std::filesystem::path directory{};
    /// Where the discovery record goes; the per-user default when empty. Tests use their own.
    std::filesystem::path discovery{};
    /// Per-operation native deadline.
    std::chrono::seconds timeout{120};
    /// How long the window may take to claim the session, counted from the activation: a renderer blocked before its first
    /// poll has no operation for `timeout` to expire. Past it the session is revoked like a deadline. Must be positive;
    /// `seconds::max()` disables the delay.
    std::chrono::seconds startupTimeout{120};
    /// After a deadline revokes the session, how long the window may take to close on its own. `window.close()` cannot run
    /// in a renderer stuck in a loop, so past this grace the listener and record are removed and `onUnresponsive` is called.
    /// Must not be negative; `milliseconds::max()` waits for the window without limit.
    std::chrono::milliseconds closeGrace{5000};
    /// What to do with a window that did not close. By default the process ends with status 72: `show()` cannot be
    /// interrupted from another thread. Set it to close the application another way.
    std::function<void()> onUnresponsive{};
    /// Whether to tell the human, on stderr, the MCP URL, the client configuration and the grants.
    bool announce = true;
};

// Clock is a time source returning steady_clock::time_point; tests advance it without sleeping through deadlines.
template <typename App, typename Clock = std::chrono::steady_clock>
class Automation {
    // Session keeps its deadlines in steady_clock::time_point: an injected clock must return that type.
    static_assert(std::is_same_v<decltype(Clock::now()), std::chrono::steady_clock::time_point>,
                  "Automation's Clock must return std::chrono::steady_clock::time_point");

public:
    /// @throws std::invalid_argument for an unknown grant, a non-positive `startupTimeout`, a negative `closeGrace`, or a
    ///         `directory` inside the project
    /// @throws std::runtime_error (including std::filesystem::filesystem_error) when the private or the discovery directory cannot
    ///         be created or is refused (an existing one that other users can read), std::system_error when the MCP port cannot
    ///         be bound, std::invalid_argument when the discovery record would exceed `discovery::recordLimit` (paths too long).
    ///         Nothing stays registered in the application.
    Automation(App& app, AutomationOptions options)
        : session(make(app, options)),
          discoveryDirectory(options.discovery.empty() ? discovery::defaultDirectory() : options.discovery),
          privateDirectory(channel::outsideProject(*session, privateDirectoryFor(options, discoveryDirectory, *session))),
          mcp(session, privateDirectory.path, options.mcpPort, options.mcpToken, !options.disableMcpAuthentication),
          registration(discoveryDirectory, record(*session, privateDirectory.path, mcp)),
          grants(std::move(options.grants)),
          callbackNames(std::move(options.callbacks)),
          authenticationDisabled(options.disableMcpAuthentication),
          grace(options.closeGrace),
          unresponsive(std::move(options.onUnresponsive)),
          launched(Clock::now()),
          startup(options.startupTimeout),
          // Last of the fallible steps: the binding registers callbacks in the application and cannot take them back, so
          // everything that can refuse the activation (grants, directory, port, record) has been validated before it.
          binding(app, session) {
        // The native deadline of a stuck operation revokes the session and asks the window to close.
        // A renderer in a loop never runs that request: after the grace the window is declared unresponsive.
        supervisor = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (session->expire(Clock::now()) || session->expireStartup(startupDeadline(launched, startup), Clock::now()))
                    session->close("Native automation deadline exceeded");
                if (session->isClosed()) {
                    // The grace starts when the window is asked to close, so a closeBrowser() that blocks does not extend it.
                    const auto deadline = deadlineAfter(Clock::now(), grace);
                    binding.closeBrowser();
                    // Whatever revoked the session (a deadline, the page's own failure report, a lost WebSocket), the window was
                    // only asked to close: a stuck renderer ignores it, and a lost
                    // connection cannot even carry the request. Only the application ending show() (close()) confirms that the
                    // native window is gone, so the grace always runs.
                    if (Object(session->describe()).string("failure") == "Developer window disconnected")
                        revoke();  // the connection is gone: no listener nor running record may remain meanwhile
                    while (!stop.stop_requested() && Clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    if (!stop.stop_requested()) {
                        revoke();
                        if (unresponsive)
                            unresponsive();
                        else
                            std::_Exit(72);
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
        });
        binding.announce(banner());  // the terminal of the window says it too, once automation is connected
        if (options.announce)
            std::cerr << banner() << '\n';
    }
    Automation(const Automation&)            = delete;
    Automation& operator=(const Automation&) = delete;
    ~Automation() {
        close();
    }

    /// The page to give to `app.show()`: it carries the single-use claim that selects this window.
    std::string launchPage() const {
        return binding.launchPage();
    }
    /// The MCP URL, `http://127.0.0.1:PORT/mcp`.
    const std::string& mcpUrl() const {
        return mcp.url();
    }
    /// The private client configuration (`mcp.json`: URL and Bearer token). Never copy it into the project: the developer
    /// file bridge can read the project.
    const std::filesystem::path& clientConfiguration() const {
        return mcp.configuration();
    }
    /// The private directory that holds the client configuration.
    const std::filesystem::path& directory() const {
        return privateDirectory.path;
    }
    /// What the human sees: the grants, the URL and the client configuration. It contains no secret.
    std::string banner() const {
        std::string list;
        for (const auto& grant : grants)
            list += (list.empty() ? "" : ", ") + grant;
        std::string names;
        for (const auto& callback : callbackNames)
            names += (names.empty() ? "" : ", ") + callback;
        const auto granted = std::string{authenticationDisabled ? "WARNING: MCP authentication disabled; local processes can use granted operations; " : ""}
                             + "grants: " + (list.empty() ? std::string{"none"} : list) + (names.empty() ? "" : "; callbacks: " + names);
        return "WebFront MCP (" + granted + "): " + mcp.url() + " - client configuration: " + mcp.configuration().generic_string();
    }
    /// `base` marked as shared with an agent, for a window title: automation must be visible to the person using it.
    std::string windowTitle(std::string_view base) const {
        return std::string{base} + " [automation]";
    }
    /// Revokes the session, closes the listener, withdraws the discovery record, the client configuration and the private
    /// directory. Idempotent; call after `show()` returns.
    void close(const std::string& reason = "Developer window closed") {
        session->close(reason);
        revoke();
        if (supervisor.joinable()) {
            supervisor.request_stop();
            if (supervisor.get_id() != std::this_thread::get_id())
                supervisor.join();
        }
    }
    const std::shared_ptr<Session>& controller() const {
        return session;
    }

private:
    // Withdraws the record, the listener and the private directory once, from whichever thread gets there first.
    void revoke() {
        const std::scoped_lock lock{revocation};
        if (std::exchange(revoked, true))
            return;
        registration.remove();
        mcp.stop();  // removes mcp.json, before the directory that holds it
        privateDirectory.cleanup();
    }
    static std::shared_ptr<Session> make(App& app, const AutomationOptions& options) {
        mcphttp::validateTokenOptions(options.mcpToken, !options.disableMcpAuthentication);
        if (options.startupTimeout <= std::chrono::seconds::zero())
            throw std::invalid_argument("Automation startupTimeout must be positive");
        if (options.closeGrace < std::chrono::milliseconds::zero())
            throw std::invalid_argument("Automation closeGrace must not be negative");
        return std::make_shared<Session>(
            SessionOptions{.project = app.projectDirectory(), .allow = options.grants, .timeout = options.timeout, .callbacks = options.callbacks});
    }
    // `directory`, or a new directory named after the session in the (private) discovery directory. The records of the
    // instances that ended without closing (a forced end) are removed first, with the private directories named after them.
    static std::filesystem::path
    privateDirectoryFor(const AutomationOptions& options, const std::filesystem::path& directoryOfRecords, const Session& session) {
        discovery::preparePrivateDirectory(directoryOfRecords);
        (void)discovery::list(directoryOfRecords);
        if (!options.directory.empty())
            return options.directory;
        return directoryOfRecords / Object(session.describe()).string("session");
    }
    static discovery::Entry record(const Session& session, const std::filesystem::path& endpoint, const mcphttp::Server<Session>& mcp) {
        const Object description{session.describe()};
        return {.session       = description.string("session"),
                .project       = description.string("project"),
                .endpoint      = endpoint.generic_string(),
                .allow         = description.raw("allow"),
                .pid           = discovery::currentProcess(),
                .callbacks     = description.raw("callbacks"),
                .mcp           = mcp.url(),
                .configuration = mcp.configuration().generic_string()};
    }

    std::shared_ptr<Session>              session;
    std::filesystem::path                 discoveryDirectory;
    channel::PrivateDirectory             privateDirectory;
    mcphttp::Server<Session>              mcp;
    discovery::Registration               registration;
    std::vector<std::string>              grants;
    std::vector<std::string>              callbackNames;
    bool                                  authenticationDisabled;
    std::chrono::milliseconds             grace;
    std::function<void()>                 unresponsive;
    std::mutex                            revocation;
    bool                                  revoked = false;
    std::chrono::steady_clock::time_point launched;
    std::chrono::seconds                  startup;
    Binding<App>                          binding;
    std::jthread                          supervisor;
};

/// Enables automation on an application; keep the result alive until after `show()` and call `close()` before
/// `shutdown()`.
template <typename App>
std::unique_ptr<Automation<App>> enableAutomation(App& app, AutomationOptions options) {
    mcphttp::validateTokenOptions(options.mcpToken, !options.disableMcpAuthentication);
    return std::make_unique<Automation<App>>(app, std::move(options));
}
}  // namespace webfront::automation
