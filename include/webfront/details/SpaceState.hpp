// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — core
// Role: The state an application space shares with its pages' connections, and the registry of an application's spaces.
// Thread: thread-safe | Alloc: unrestricted
// LLM: internal to BasicWF and BasicSpace; the lock that guards each member is named beside it

#pragma once

#include "../http/HTTPServer.hpp"
#include "../UISubscribers.hpp"
#include "../weblink/WebLink.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront {

namespace detail {
/// What a space shares with the connections of its pages: they keep it alive after the space is destroyed, which the
/// `alive` flag tells them. `Handler` is the type-erased C++ function of the application.
template <typename UI, typename Handler>
struct SpaceState {
    SpaceState(std::string spacePath, std::string spaceSegment) : path(std::move(spacePath)), segment(std::move(spaceSegment)) { }

    const std::string path;     ///< "/s/abc/": the mount's prefix, the segment and a slash.
    const std::string segment;  ///< "abc".
    /// Cleared, under the application's lock of its connections, when the space is destroyed: no page joins it afterwards.
    std::atomic<bool> alive{true};
    /// The pages connected and not closed; guarded by the application's lock of its connections.
    std::size_t pages = 0;

    /// Every call of the space's route handlers and of their replies' bodies runs through it: destroying the space retires
    /// it, which waits for those running on the server's workers and lets none start afterwards.
    const std::shared_ptr<HandlerGuard> routeGuard = std::make_shared<HandlerGuard>();

    const std::shared_ptr<UISubscribers<UI>> started = std::make_shared<UISubscribers<UI>>();
    const std::shared_ptr<UISubscribers<UI>> closed  = std::make_shared<UISubscribers<UI>>();

    mutable std::mutex                                           mutex;  // guards the members below
    std::map<std::string, std::shared_ptr<Handler>, std::less<>> functions;
    // Shared, so that each notification calls the very handler that was set (its state persists between calls), outside the
    // lock, while the handler may replace itself or destroy the space.
    std::shared_ptr<std::function<void(UI)>> startedHandler;
    std::shared_ptr<std::function<void()>>   lastClosedHandler;
    std::shared_ptr<http::RouteTable>        routes = std::make_shared<http::RouteTable>();

    [[nodiscard]] std::shared_ptr<Handler> function(std::string_view name) const {
        std::scoped_lock lock{mutex};
        const auto       found = functions.find(name);
        return found == functions.end() ? nullptr : found->second;
    }
    [[nodiscard]] std::shared_ptr<std::function<void(UI)>> onStarted() const {
        std::scoped_lock lock{mutex};
        return startedHandler;
    }
    [[nodiscard]] std::shared_ptr<std::function<void()>> onLastClosed() const {
        std::scoped_lock lock{mutex};
        return lastClosedHandler;
    }
    [[nodiscard]] std::shared_ptr<http::RouteTable> routeTable() const {
        std::scoped_lock lock{mutex};
        return routes;
    }

    /// Drops what the application registered; the handlers are destroyed outside the lock, as their captures may take it.
    void release() {
        std::map<std::string, std::shared_ptr<Handler>, std::less<>> droppedFunctions;
        std::shared_ptr<std::function<void(UI)>>                     droppedStarted;
        std::shared_ptr<std::function<void()>>                       droppedLastClosed;
        std::shared_ptr<http::RouteTable>                            droppedRoutes;
        std::scoped_lock                                             lock{mutex};
        droppedFunctions.swap(functions);
        droppedStarted.swap(startedHandler);
        droppedLastClosed.swap(lastClosedHandler);
        droppedRoutes.swap(routes);
    }
};

/// The spaces of an application, shared with each of them: a space destroyed after its application finds it detached.
template <typename State>
struct SpaceRegistry {
    std::mutex                                                 mutex;
    bool                                                       attached = true;  // false once the application is destroyed
    std::vector<std::string>                                   families;         // the prefixes given to BasicWF::spaces
    std::map<std::string, std::shared_ptr<State>, std::less<>> spaces;           // by path, "/s/abc/"

    [[nodiscard]] std::shared_ptr<State> find(std::string_view path) {
        std::scoped_lock lock{mutex};
        const auto       found = spaces.find(path);
        return found == spaces.end() ? nullptr : found->second;
    }
};

/// True for a segment that names a space: the rule of the mount, so that every space created is reachable.
[[nodiscard]] inline bool validSpaceSegment(std::string_view segment) {
    return http::routing::validSegment(segment);
}
}  // namespace detail

}  // namespace webfront
