// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The toolkit-independent part of the native frontends' windows: the registry of the secondary windows and the
/// session of one open() (#652, #616).
#pragma once

#include "NativeWindow.hpp"
#include "UIExecutor.hpp"

#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::frontend::detail {

/**
 * @brief The windows of the application beside the first one. Every member is used on the interface thread only.
 *
 * `Session` is a frontend's window with a `bool closed`, a `std::exception_ptr failure` (why it closed, if it did not
 * end normally) and a `std::function<void()> closedHook` that it calls once, from its own callback, when its window closes.
 *
 * `Hooks` holds what depends on the toolkit (it is a member, `hooks`, so it can keep state):
 * - `static constexpr std::string_view name`: the frontend's name, the prefix of its messages;
 * - `void scheduleReap(Registry&)`: arranges for `reap()` to run later, outside of any callback of the retired
 *   session (an idle callback, a queued block, a message of the first window);
 * - `void cancelReap()`: withdraws what scheduleReap() arranged, when the first window ended;
 * - `void present(Session&)`: brings the window to the front, as it should (a hidden one stays hidden);
 * - `bool close(Session&)`: asks the window to close as its user would, false when it cannot.
 */
template <typename Session, typename Hooks>
class WindowRegistry {
public:
    /// The first window, `nullptr` when none runs.
    Session*                                        primary = nullptr;
    std::map<std::string, std::shared_ptr<Session>> secondary;
    std::vector<std::shared_ptr<Session>>           finished;  // closed, destroyed by reap()
    Hooks                                           hooks;

    /// A session closed (its closedHook): takes it out of the table, reports why it failed, and reaps it later.
    void retire(const std::string& page) {
        const auto found = secondary.find(page);
        if (found == secondary.end())
            return;
        // open() returned long ago, so nothing else can report a page that failed (navigation, runtime, process lost):
        // the failure is written, then the window is reaped.
        if (found->second->failure) {
            try {
                std::rethrow_exception(found->second->failure);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "%.*s: the secondary window of '%s' closed: %s\n", static_cast<int>(Hooks::name.size()), Hooks::name.data(), page.c_str(), error.what());
            } catch (...) {
            }
        }
        finished.push_back(std::move(found->second));
        secondary.erase(found);
        hooks.scheduleReap(*this);
    }

    /// Destroys the sessions that closed, outside of any of their own callbacks (it is what scheduleReap() arranged).
    void reap() {
        auto closed = std::move(finished);
        finished.clear();
        closed.clear();
    }

    /// The first window ended: the others end with it, and nothing is left to reap. The sessions leave the table before
    /// they are destroyed, and their hook no longer retires them from it.
    void clear() {
        hooks.cancelReap();
        auto windows = std::move(secondary);
        secondary.clear();
        for (auto& entry : windows)
            entry.second->closedHook = nullptr;
        finished.clear();
        windows.clear();
        primary = nullptr;
    }

    /**
     * @brief On the interface thread: opens `page` in a window of its own, or brings the window already showing it to the front.
     *
     * `create(Session& primary)` builds the window and returns a `std::shared_ptr<Session>`; `start(Session&, Session& primary)`
     * starts its load and returns. Either may throw (a window whose creation or start throws is not registered, and its page
     * can be opened again). Throws std::logic_error when the first window is not open: one that closed still runs its loop
     * until the pass ends, and a window built now would be destroyed with it.
     */
    template <typename Create, typename Start>
    void open(const std::string& page, Create&& create, Start&& start) {
        if (!primary || primary->closed)
            throw std::logic_error(std::string(Hooks::name) + ": no window of the application is open; show() it before opening another");
        if (const auto found = secondary.find(page); found != secondary.end()) {
            hooks.present(*found->second);
            return;
        }
        std::shared_ptr<Session> session = create(*primary);
        session->closedHook              = [this, page] {
            retire(page);
        };
        start(*session, *primary);
        // One that closed during start() retired itself from a table that did not hold it yet, without a word: the caller
        // is still waiting, so the failure is thrown to it.
        if (session->closed) {
            if (session->failure)
                std::rethrow_exception(session->failure);
            throw std::runtime_error(std::string(Hooks::name) + ": the secondary window of '" + page + "' closed while it started");
        }
        secondary[page] = std::move(session);
    }

    /// On the interface thread: asks the secondary window showing `page` to close, as its user would. False when there is none.
    bool close(const std::string& page) {
        const auto found = secondary.find(page);
        if (found == secondary.end() || found->second->closed)
            return false;
        const auto session = found->second;  // kept alive while its callback retires it
        return hooks.close(*session);
    }
};

/**
 * @brief What an open() holds from the moment its loop is attached until it returns, in the order that never leaves
 * a dangling window: attached and published on construction; on destruction (also by exception) the secondary windows
 * are cleared, then the window is withdrawn, then the host closes (pending operations complete stopped).
 */
template <typename Registry>
class InterfaceSession {
    UIExecutorHost&   host;
    NativeWindowHost& windowHost;
    Registry&         windows;

public:
    /// Refuses a second open() while one runs: a process opens one window at a time.
    static void refuseSecond(const UIExecutorHost& host, std::string_view name) {
        if (host.executor().available())
            throw std::logic_error(std::string(name) + ": open() is already running; a process opens one window at a time");
    }

    /// Attaches `wake` (thread-safe; makes the loop call host.drain()), publishes `handle` and registers `primary`
    /// as the first window. Throws what attach() does (a loop is already attached) before anything is held.
    InterfaceSession(UIExecutorHost& uiHost, NativeWindowHost& nativeHost, Registry& registry, typename std::remove_pointer_t<decltype(Registry::primary)>& primary, std::function<void()> wake, NativeWindowHandle handle)
        : host(uiHost), windowHost(nativeHost), windows(registry) {
        host.attach(std::move(wake));
        try {
            windowHost.publish(std::move(handle));
            windows.primary = &primary;
        } catch (...) {
            windowHost.retract();
            host.close();
            throw;
        }
    }
    InterfaceSession(const InterfaceSession&)            = delete;
    InterfaceSession& operator=(const InterfaceSession&) = delete;
    ~InterfaceSession() {
        windows.clear();
        windowHost.retract();
        host.close();
    }
};

}  // namespace webfront::frontend::detail
