// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-23
//
// WebFront — frontend
// Role: The toolkit-independent registry of the secondary windows and the session of one open().
// Thread: ui-loop | Alloc: heap-init
// LLM: shared by the native frontends; mutated on the interface thread only

// The toolkit-independent part of the native frontends' windows: the registry of the secondary windows and the
// session of one open() (#652, #616).

#pragma once

#include "NativeWindow.hpp"
#include "UIExecutor.hpp"

#include <algorithm>
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
 * A window that closes in two steps (WebView2 ends its page first, #708) also has a `std::function<void()> closingHook`,
 * which it calls once when the first step starts: the window leaves the table at once, so that its page can be opened
 * again, and stays alive until its closedHook.
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
    // Its hooks no longer reach the registry: the session is destroyed with it.
    static void forget(Session& session) {
        session.closedHook = nullptr;
        if constexpr (requires { session.closingHook; })
            session.closingHook = nullptr;
    }

public:
    /// The first window, `nullptr` when none runs.
    Session*                                        primary = nullptr;
    std::map<std::string, std::shared_ptr<Session>> secondary;
    std::vector<std::shared_ptr<Session>>           finished;          // closed, destroyed by reap()
    std::vector<std::shared_ptr<Session>>           closing;           // out of the table, closing: kept until their closedHook
    Hooks                                           hooks;
    bool                                            clearing = false;  // clear() is destroying the windows

    /// The session `which` started to close in two steps (its closingHook): it leaves the table, so that a new open() of
    /// its page creates another window, and is kept until it closes.
    void detach(const Session* which) {
        for (auto entry = secondary.begin(); entry != secondary.end(); ++entry)
            if (entry->second.get() == which) {
                closing.push_back(std::move(entry->second));
                secondary.erase(entry);
                return;
            }
    }

    /// The session `which`, showing `page`, closed (its closedHook): takes it out of the table, or out of the closing
    /// windows, reports why it failed, and reaps it later. Found by identity: a window opened since for the same page stays.
    void retire(const std::string& page, const Session* which) {
        std::shared_ptr<Session> session;
        if (const auto found = secondary.find(page); found != secondary.end() && found->second.get() == which) {
            session = std::move(found->second);
            secondary.erase(found);
        } else if (const auto ending = std::ranges::find(closing, which, &std::shared_ptr<Session>::get); ending != closing.end()) {
            session = std::move(*ending);
            closing.erase(ending);
        } else
            return;
        // open() returned long ago, so nothing else can report a page that failed (navigation, runtime, process lost):
        // the failure is written, then the window is reaped.
        if (session->failure) {
            try {
                std::rethrow_exception(session->failure);
            } catch (const std::exception& error) {
                std::fprintf(stderr,
                             "%.*s: the secondary window of '%s' closed: %s\n",
                             static_cast<int>(Hooks::name.size()),
                             Hooks::name.data(),
                             page.c_str(),
                             error.what());
            } catch (...) { }
        }
        finished.push_back(std::move(session));
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
        clearing = true;
        hooks.cancelReap();
        auto windows = std::move(secondary);
        secondary.clear();
        auto ending = std::move(closing);
        closing.clear();
        for (auto& entry : windows)
            forget(*entry.second);
        for (auto& session : ending)
            forget(*session);
        finished.clear();
        windows.clear();
        ending.clear();
        primary  = nullptr;
        clearing = false;
    }

    /**
     * @brief On the interface thread: opens `page` in a window of its own, or brings the window already showing it to the front.
     *
     * `create(Session& primary)` builds the window and returns a `std::shared_ptr<Session>`; `start(Session&, Session& primary)`
     * starts its load and returns. Either may throw (a window whose creation or start throws is not registered, and its page
     * can be opened again). A page already showing a window is brought to the front. Throws std::logic_error when a new window is needed and the first window
     * is not open: one that closed still runs its loop until the pass ends, and a window built now would be destroyed with it. False, with a word on standard
     * error, when the session is ending (clear() runs): the request comes from the teardown of a window, which an exception would turn into the end of the
     * process, so it is refused without one.
     */
    template <typename Create, typename Start>
    bool open(const std::string& page, Create&& create, Start&& start) {
        if (clearing) {
            std::fprintf(stderr,
                         "%.*s: the window of '%s' was not opened: the session is ending\n",
                         static_cast<int>(Hooks::name.size()),
                         Hooks::name.data(),
                         page.c_str());
            return false;
        }
        if (const auto found = secondary.find(page); found != secondary.end()) {
            hooks.present(*found->second);
            return true;
        }
        if (!primary || primary->closed)
            throw std::logic_error(std::string(Hooks::name) + ": no window of the application is open; show() it before opening another");
        std::shared_ptr<Session> session = create(*primary);
        session->closedHook              = [this, page, which = session.get()] {
            retire(page, which);
        };
        if constexpr (requires { session->closingHook; })
            session->closingHook = [this, which = session.get()] {
                detach(which);
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
        return true;
    }

    /// On the interface thread: asks the secondary window showing `page` to close, as its user would. False when there is
    /// none (a window that closes in two steps is no longer there once the first step started).
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
    InterfaceSession(UIExecutorHost&                                              uiHost,
                     NativeWindowHost&                                            nativeHost,
                     Registry&                                                    registry,
                     typename std::remove_pointer_t<decltype(Registry::primary)>& primary,
                     std::function<void()>                                        wake,
                     NativeWindowHandle                                           handle)
        : host(uiHost), windowHost(nativeHost), windows(registry) {
        host.attach(std::move(wake));
        bool published = false;
        try {
            windowHost.publish(std::move(handle));
            published       = true;
            windows.primary = &primary;
        } catch (...) {
            if (published)  // only what this constructor did: a publish that threw left another window's handle in place
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
