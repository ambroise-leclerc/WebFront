/// @brief Opt-in automation of a consumer's own developer window.
///
/// `enableAutomation(app, options)` mounts the controller (`Session`), the page binding (`Binding`) and the private
/// attachment endpoint on an application that already called `bootstrap("developer")`, then publishes a discovery
/// record. `webfront-agent --attach <listen>` then drives that application's own window: check, build, test, preview,
/// inspect and interact run in the page of the consumer, whose `cppFunction` callbacks stay registered. Nothing is
/// registered unless this function is called, and the grants are fixed here by the native owner.
///
/// ```cpp
/// webfront::NativeApp app{project};
/// app.bootstrap("developer");
/// app.cppFunction("bf.render", render);
/// auto automation = webfront::automation::enableAutomation(
///     app, {.grants = {"check", "build", "inspect", "interact"}, .listen = privateDirectory});
/// app.show(automation->launchPage());
/// automation->close();
/// app.shutdown();
/// ```
#pragma once
#include "Binding.hpp"
#include "Channel.hpp"
#include "Discovery.hpp"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

namespace webfront::automation {
struct AutomationOptions {
    /// Granted operations (`check`, `build`, `test`, `preview`, `inspect`, `interact`, and the setup grants). None
    /// leaves only session discovery.
    std::vector<std::string> grants{};
    /// The application callbacks the `call` grant may invoke (`callbacks = {"bf.render"}`): an explicit list of names, never a
    /// pattern. The agent sees the names, gives them one string at most and reads a scalar or list back; `webfront.*` names are
    /// refused. Needs the `call` grant; without it nothing of the application can be called.
    std::vector<std::string> callbacks{};
    /// A new private directory outside the project that receives the attachment endpoint.
    std::filesystem::path listen{};
    /// Where the discovery record goes; the per-user default when empty. Tests use their own.
    std::filesystem::path discovery{};
    /// Per-operation native deadline.
    std::chrono::seconds timeout{120};
    /// How long the window may take to claim the session, counted from the activation: a renderer blocked before its first
    /// poll has no operation for `timeout` to expire. Past it the session is revoked like a deadline.
    std::chrono::seconds startupTimeout{120};
    /// After a deadline revokes the session, how long the window may take to close on its own. `window.close()` cannot run
    /// in a renderer stuck in a loop, so past this grace the endpoint and record are removed and `onUnresponsive` is called.
    std::chrono::milliseconds closeGrace{5000};
    /// What to do with a window that did not close. By default the process ends with status 72 (as `webfront-agent`
    /// does): `show()` cannot be interrupted from another thread. Set it to close the application another way.
    std::function<void()> onUnresponsive{};
    /// Whether to tell the human, on stderr, that an agent can attach and with which grants.
    bool announce = true;
};

// Clock is a time source returning steady_clock::time_point; tests advance it without sleeping through deadlines.
template <typename App, typename Clock = std::chrono::steady_clock>
class Automation {
    // Session keeps its deadlines in steady_clock::time_point: an injected clock must return that type.
    static_assert(std::is_same_v<decltype(Clock::now()), std::chrono::steady_clock::time_point>,
                  "Automation's Clock must return std::chrono::steady_clock::time_point");

public:
    /// @throws std::invalid_argument for an unknown grant, an empty `listen` path, or a `listen` directory inside the project
    /// @throws std::runtime_error (including std::filesystem::filesystem_error) when the endpoint or the discovery directory cannot be
    ///         created or is refused (an existing one that other users can read). Nothing stays registered in the application.
    Automation(App& app, AutomationOptions options)
        : session(make(app, options)),
          channel(session, options.listen),
          registration(options.discovery.empty() ? discovery::defaultDirectory() : options.discovery, record(*session, channel.directory())),
          grants(std::move(options.grants)),
          callbackNames(std::move(options.callbacks)),
          endpointDirectory(channel.directory()),
          grace(options.closeGrace),
          unresponsive(std::move(options.onUnresponsive)),
          launched(Clock::now()),
          startup(options.startupTimeout),
          // Last of the fallible steps: the binding registers callbacks in the application and cannot take them back, so
          // everything that can refuse the activation (grants, endpoint, record) has been validated before it.
          binding(app, session) {
        // The native deadline of a stuck operation revokes the session and asks the window to close, as the launcher does.
        // A renderer in a loop never runs that request: after the grace the window is declared unresponsive.
        supervisor = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (session->expire(Clock::now()) || session->expireStartup(launched + startup, Clock::now()))
                    session->close("Native automation deadline exceeded");
                if (session->isClosed()) {
                    // The grace starts when the window is asked to close, so a closeBrowser() that blocks does not extend it.
                    const auto deadline = Clock::now() + grace;
                    binding.closeBrowser();
                    // Whatever revoked the session (a deadline, a signal, an attached client's --shutdown, the page's own failure
                    // report, a lost WebSocket), the window was only asked to close: a stuck renderer ignores it, and a lost
                    // connection cannot even carry the request. Only the application ending show() (close()) confirms that the
                    // native window is gone, so the grace always runs.
                    if (Object(session->describe()).string("failure") == "Developer window disconnected")
                        revoke();  // the connection is gone: no endpoint nor running record may remain meanwhile
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
    /// The endpoint directory to give to `webfront-agent --attach`.
    const std::filesystem::path& endpoint() const {
        return endpointDirectory;
    }
    /// What the human sees: the grants and how to attach. It contains no secret.
    std::string banner() const {
        std::string list;
        for (const auto& grant : grants)
            list += (list.empty() ? "" : ", ") + grant;
        std::string names;
        for (const auto& callback : callbackNames)
            names += (names.empty() ? "" : ", ") + callback;
        return "WebFront automation enabled (grants: " + (list.empty() ? std::string{"none"} : list) + (names.empty() ? "" : "; callbacks: " + names) + "). Attach with: webfront-agent --attach \""
               + endpointDirectory.generic_string() + "\" --mcp";
    }
    /// `base` marked as shared with an agent, for a window title: automation must be visible to the person using it.
    std::string windowTitle(std::string_view base) const {
        return std::string{base} + " [automation]";
    }
    /// Revokes the session, withdraws the discovery record and the endpoint. Idempotent; call after `show()` returns.
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
    // Withdraws the record and the endpoint once, from whichever thread gets there first.
    void revoke() {
        const std::scoped_lock lock{revocation};
        if (std::exchange(revoked, true))
            return;
        registration.remove();
        channel.stop();
    }
    static std::shared_ptr<Session> make(App& app, const AutomationOptions& options) {
        if (options.listen.empty())
            throw std::invalid_argument("Automation needs a new private `listen` directory for its endpoint");
        return std::make_shared<Session>(SessionOptions{.project = app.projectDirectory(), .allow = options.grants, .timeout = options.timeout, .callbacks = options.callbacks});
    }
    static discovery::Entry record(const Session& session, const std::filesystem::path& endpoint) {
        const Object description{session.describe()};
        return {.session  = description.string("session"),
                .project  = description.string("project"),
                .endpoint = endpoint.generic_string(),
                .allow    = description.raw("allow"),
                .pid      = discovery::currentProcess(),
                .callbacks = description.raw("callbacks")};
    }

    std::shared_ptr<Session>      session;
    ChannelServer                 channel;
    discovery::Registration       registration;
    std::vector<std::string>      grants;
    std::vector<std::string>      callbackNames;
    std::filesystem::path         endpointDirectory;
    std::chrono::milliseconds     grace;
    std::function<void()>         unresponsive;
    std::mutex                    revocation;
    bool                          revoked = false;
    std::chrono::steady_clock::time_point launched;
    std::chrono::seconds          startup;
    Binding<App>                  binding;
    std::jthread                  supervisor;
};

/// Enables automation on an application; keep the result alive until after `show()` and call `close()` before
/// `shutdown()`.
template <typename App>
std::unique_ptr<Automation<App>> enableAutomation(App& app, AutomationOptions options) {
    return std::make_unique<Automation<App>>(app, std::move(options));
}
}  // namespace webfront::automation
