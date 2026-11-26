// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — core
// Role: Application spaces (BasicWF::spaces): an independent set of functions, page handlers and routes under a path prefix.
// Thread: thread-safe | Alloc: unrestricted
// LLM: public API; read the BasicSpace contract, then BasicWF::spaces and its Space members, before changing the dispatch

#pragma once

#include "WebFront.hpp"

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace webfront {

/**
 * @brief One user's part of an application that serves several from one process (BasicWF::spaces): the pages loaded under
 * `prefix + segment + "/"` and their own C++ functions, page handlers and routes.
 *
 * A space offers the members of the application that concern its pages, with the same signatures, so code templated on
 * the application type (`template <typename App> class Bridge`) attaches to a space unchanged. Its UI type is the
 * application's. What a space registers is seen only by its pages: a function of the same name in two spaces answers
 * each space independently, a space's broadcast and forEachUI reach only its pages, and its routes (relative to the
 * space: addRoute("/data/") serves `/s/abc/data/`) only its paths. A function of the application stays visible to every
 * space, a space's own taking precedence; the application's handlers (onUIStarted, subscribeUIStarted,
 * subscribeUIClosed), forEachUI and broadcast concern every page, those of the spaces included.
 *
 * The application owns its spaces and decides when to destroy one: onLastUIClosed tells it when the last page closed,
 * and it may keep the space through a grace delay (a task of app.spawn awaiting app.resumeAfter). Destroying a space
 * closes its pages (the application's subscribeUIClosed handlers are told; the space's are not, being released),
 * unregisters its functions and routes, and lets the resolver decide again for its segment; destroyed by one of its handlers,
 * it calls none of its handlers that would have followed. Destroy it on the server
 * thread, where its callbacks run, or once none of them can run: a callback in progress keeps what it captured but not
 * what it refers to. Destroy the spaces before their application; one that outlives it only releases what it holds.
 */
template <typename App>
class BasicSpace {
public:
    using Net              = typename App::Net;
    using BufferPolicy     = typename App::BufferPolicy;
    using FrontendProvider = typename App::FrontendProvider;
    using UI               = typename App::UI;

    /// Creates the space `segment` of the family `prefix`, declared with app.spaces(prefix, resolver): its pages are served
    /// from now on. Typically called by that resolver. Throws std::invalid_argument for an undeclared family or a segment
    /// that cannot name a space, and std::logic_error when the space exists already.
    BasicSpace(App& application, std::string_view prefix, std::string_view segment)
        : app(application), registry(application.spaceRegistry), state(application.attachSpace(prefix, segment)) { }

    BasicSpace(const BasicSpace&)            = delete;
    BasicSpace& operator=(const BasicSpace&) = delete;

    ~BasicSpace() {
        bool attached = false;
        {
            std::scoped_lock lock{registry->mutex};
            attached = registry->attached;
            if (attached)
                registry->spaces.erase(state->path);
        }
        if (!attached) {
            // Outside the registry's lock: releasing a capture may destroy another space, which takes it.
            state->routeGuard->retire();
            state->release();
            return;
        }
        app.detachSpace(state);
    }

    /// The segment that names the space: "abc" for `/s/abc/`.
    [[nodiscard]] std::string_view segment() const noexcept {
        return state->segment;
    }
    /// The path of the space, which its pages and routes are under: "/s/abc/".
    [[nodiscard]] std::string_view path() const noexcept {
        return state->path;
    }
    [[nodiscard]] App& application() const noexcept {
        return app;
    }

    /// As BasicWF::cppFunction, for the pages of this space only; a function of the application of the same name is hidden
    /// from them. Thread-safe. @return false when the space already has a function of that name, which stays registered
    /// @throws std::invalid_argument for a name starting with `webfront.inspector.`: a space can neither hide a function of
    /// the inspectors from its pages nor pose as one (ADR-027, section 1)
    template <typename R, typename... Args>
    bool cppFunction(std::string functionName, auto&& function) {
        if (inspect::isReservedName(functionName))
            throw std::invalid_argument("webfront::BasicSpace::cppFunction(): names starting with 'webfront.inspector.' are reserved to WebFront");
        auto handler = std::make_shared<typename App::CppHandler>(app.template makeCppHandler<R, Args...>(std::forward<decltype(function)>(function)));
        std::scoped_lock lock{state->mutex};
        return state->functions.try_emplace(std::move(functionName), std::move(handler)).second;
    }

    /// As BasicWF::cppFunction, deducing the signature from the callable.
    template <typename Function>
    bool cppFunction(std::string functionName, Function&& function) {
        using Callable = std::remove_cvref_t<Function>;
        static_assert(detail::has_callable_traits<Callable>,
                      "webfront::BasicSpace::cppFunction: could not deduce this callable's signature. Generic "
                      "lambdas, overloaded operator(), and other advanced callables require explicit "
                      "cppFunction<ReturnType, ArgTypes...>(name, callable).");
        using Traits = detail::callable_traits<Callable>;
        using Args   = typename detail::caller_aware_args<UI, typename Traits::RawArgs, typename Traits::DecayedArgs>::type;
        return detail::ApplyArgs<typename Traits::Return, Args>::invoke(*this, std::move(functionName), std::forward<Function>(function));
    }

    /// Whether the pages of this space reach a C++ function of that name: the space's own or the application's.
    [[nodiscard]] bool hasCppFunction(std::string_view functionName) const {
        return state->function(functionName) != nullptr || app.hasCppFunction(functionName);
    }

    /// Sets the space's handler for each of its pages, replacing any previous one; called after the application's handlers.
    void onUIStarted(std::function<void(UI)>&& handler) {
        std::shared_ptr<std::function<void(UI)>> previous;  // destroyed outside the lock
        std::scoped_lock                         lock{state->mutex};
        previous = std::exchange(state->startedHandler, handler ? std::make_shared<std::function<void(UI)>>(std::move(handler)) : nullptr);
    }

    /// As BasicWF::subscribeUIStarted, for the pages of this space.
    [[nodiscard]] UISubscription subscribeUIStarted(std::function<void(UI)> handler) {
        return state->started->subscribe(std::move(handler));
    }

    /// As BasicWF::subscribeUIClosed, for the pages of this space; called before the application's handlers.
    [[nodiscard]] UISubscription subscribeUIClosed(std::function<void(UI)> handler) {
        return state->closed->subscribe(std::move(handler));
    }

    /// Sets the handler called on the server thread when the last connected page of the space closes, after the closed
    /// handlers: once each time the space empties, not when it is destroyed nor when the application shuts down. A space
    /// that no page ever reached is not reported. The handler may destroy the space.
    void onLastUIClosed(std::function<void()> handler) {
        std::shared_ptr<std::function<void()>> previous;  // destroyed outside the lock
        std::scoped_lock                       lock{state->mutex};
        previous = std::exchange(state->lastClosedHandler, handler ? std::make_shared<std::function<void()>>(std::move(handler)) : nullptr);
    }

    /// As BasicWF::addRoute, with a prefix relative to the space: "/data/" answers `/s/abc/data/...`, and the handler
    /// receives the path within the space (RouteRequest::path starts with "/data/"). The space's routes precede the
    /// application's. The handler and its reply's body run on the server's workers: destroying the space waits for those
    /// running (so neither may wait for the server thread, where the space is destroyed), then unregisters the routes; a
    /// request that arrives afterwards is not found, and a body still streaming ends early.
    [[nodiscard]] http::RouteTable::Registration addRoute(std::string prefix, http::RouteHandler handler) {
        return routes().add(std::move(prefix), guarded(std::move(handler)));
    }
    [[nodiscard]] http::RouteTable::Registration addRoute(std::string prefix, http::RouteHandler handler, http::RouteOptions options) {
        if (options.harden)
            options.harden = [guard = state->routeGuard, harden = std::move(options.harden)](http::RouteReply& reply) {
                guard->run([&] {
                    harden(reply);  // not once the space is destroyed: the error answer then goes unhardened
                });
            };
        return routes().add(std::move(prefix), guarded(std::move(handler)), std::move(options));
    }

    /// As BasicWF::forEachUI, for the started pages of this space.
    template <std::invocable<UI> Function>
    void forEachUI(Function&& function) {
        app.forEachUIIn(state.get(), std::forward<Function>(function));
    }

    /// As BasicWF::broadcast, for the started pages of this space.
    [[nodiscard]] Broadcast<BasicSpace> broadcast(std::string_view functionName, BroadcastOptions options = {}) {
        return Broadcast<BasicSpace>{functionName, *this, options};
    }

    /// The pages of this space connected and not closed, those in their handshake or handlers included.
    [[nodiscard]] std::size_t connectedBrowsers() const {
        return app.browsersIn(state.get());
    }

    /// As BasicWF::spawn: the task belongs to the application, and outlives the space unless the space's owner stops it.
    TaskHandle spawn(Task task) {
        return app.spawn(std::move(task));
    }

    template <typename Rep, typename Period>
    [[nodiscard]] auto resumeAfter(std::chrono::duration<Rep, Period> duration) {
        return app.resumeAfter(duration);
    }

private:
    template <typename>
    friend class Broadcast;

    void sendToStartedLinks(const detail::BroadcastCall<Net>& call) {
        app.sendToStartedLinks(call, state.get());
    }

    /// The handler, and the body of each reply it gives, run through the space's route guard.
    http::RouteHandler guarded(http::RouteHandler handler) const {
        if (!handler)
            return handler;  // refused by RouteTable::add
        return [guard = state->routeGuard, handler = std::move(handler)](const http::RouteRequest& request) {
            std::optional<http::RouteReply> reply;
            guard->run([&] {
                reply = handler(request);
            });
            if (!reply)
                return http::textReply(http::Response::notFound, "Not Found\n");  // the space is destroyed
            if (reply->body)
                reply->body = [guard, body = std::move(reply->body)](std::span<char> out) {
                    std::optional<std::size_t> count;
                    guard->run([&] {
                        count = body(out);
                    });
                    if (!count)
                        throw std::runtime_error("The space of this route was destroyed");
                    return *count;
                };
            return std::move(*reply);
        };
    }

    http::RouteTable& routes() {
        auto table = state->routeTable();
        if (!table)
            throw std::logic_error("webfront::BasicSpace: the space is released");
        return *table;  // owned by the state, which this space keeps
    }

    App&                                             app;
    std::shared_ptr<typename App::SpaceRegistryType> registry;
    std::shared_ptr<typename App::SpaceState>        state;
};

}  // namespace webfront
