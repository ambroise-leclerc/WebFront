// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once

#include "Broadcast.hpp"
#include "dom/Document.hpp"
#include "frontend/Frontend.hpp"
#include "http/HTTPServer.hpp"
#include "JsFunction.hpp"
#include "JsFunctionSender.hpp"
#include "networking/TCPNetworkingStdnet.hpp"
#include "system/DefaultFS.hpp"
#include "system/IndexFS.hpp"
#include "system/WindowsCompat.hpp"
#include "Task.hpp"
#include "UISubscribers.hpp"
#include "weblink/DomBatch.hpp"
#include "weblink/Messages.hpp"
#include "weblink/WebLink.hpp"

#include <chrono>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront {

inline constexpr std::string_view version = "0.5.0";

namespace testing {
template <typename Application>
struct ConnectionIds;
}  // namespace testing

using NetProvider     = networking::TCPStdnet;
using ConnectionError = std::runtime_error;

template <typename WebFront>
class BasicUI {
    WebFront* webFront; ///< Never null: the only constructor takes a reference, and there is no default constructor.
    WebLinkId webLinkId;
    WebLinkGeneration generation;

public:
    BasicUI(WebFront& wf, WebLinkId id, WebLinkGeneration connectionGeneration) : webFront(&wf), webLinkId(id), generation(connectionGeneration) {}

    /// Identifies this browser's connection among those connected. The 16-bit identifier of a released connection
    /// is reused after 65,536 later connections; the value also carries the connection's generation, which is never
    /// reused, so id() alone does not tell two connections apart over time: compare values instead.
    [[nodiscard]] WebLinkId id() const noexcept {
        return webLinkId;
    }

    /// Two values are equal when they belong to the same application, have the same id() and the same connection
    /// generation. A value never refers to a connection other than its own: once that connection closes, the value
    /// reaches no browser (calls are dropped, typed calls fail, pendingWrites() is 0), even after its identifier is
    /// reused, and it equals no later connection's value. Removing values in subscribeUIClosed is still advisable, to
    /// free them and keep stale ones out of an "authorized clients" list. A value must not outlive its application:
    /// comparing or using it afterwards reads a dangling application address, whose result is meaningless. Assigning a
    /// value is not atomic: a container of values shared between threads needs a lock.
    [[nodiscard]] friend bool operator==(const BasicUI& a, const BasicUI& b) noexcept {
        return a.webFront == b.webFront && a.webLinkId == b.webLinkId && a.generation == b.generation;
    }

    /**
     * @brief Injects a Javascript script in the client
     *
     * @param script
     */
    void addScript(std::string_view script) const {
        const auto link = webFront->findLink(webLinkId, generation);
        if (!link)
            throw ConnectionError("Connection with client lost");
        link->sendCommand(msg::TextCommand(msg::TxtOpcode::injectScript, script));
    }

    /// Queues one DOM command on the bridge context. Rejections arrive at wfDom.rejected(batchId, reason).
    void sendDomBatch(const msg::DomBatch& batch) const {
        const auto link = webFront->findLink(webLinkId, generation);
        if (!link) throw ConnectionError("Connection with client lost");
        link->sendCommand(batch);
    }

    /// One shared DOM document for this connection; call only on the server thread.
    [[nodiscard]] auto document() const {
        return dom::Document<BasicUI>{*this};
    }

private:
    template <typename>
    friend class dom::Document;
    [[nodiscard]] auto domState() const {
        return webFront->domState(webLinkId, generation);
    }

public:
    /// The frames queued for this browser and not yet written to the network; 0 once its connection is gone.
    [[nodiscard]] std::size_t pendingWrites() const {
        const auto link = webFront->findLink(webLinkId, generation);
        return link ? link->pendingWrites() : 0;
    }

    /**
     * @brief Creates a Javascript function object.
     *
     * @param functionName
     * @return JsFunction<WebFront>
     */
    [[nodiscard]] JsFunction<WebFront> jsFunction(std::string_view functionName) const {
        return JsFunction{functionName, *webFront, webLinkId, generation};
    }

    template <typename R>
    [[nodiscard]] JsFunction<WebFront, R> jsFunction(std::string_view functionName) const {
        return JsFunction<WebFront, R>{functionName, *webFront, webLinkId, generation};
    }

    /** @brief Starts a coroutine owned by the application; the returned handle can cancel it. */
    TaskHandle spawn(Task task) const {
        return webFront->spawn(std::move(task));
    }

    /// A sender completing once at most `limit` frames await the network for this browser, or at once when it is gone;
    /// co_await propagates the task's stop token. It paces output to a browser that reads slower than it is written.
    [[nodiscard]] auto writesAtMost(std::size_t limit) const {
        using Link = typename decltype(webFront->findLink(webLinkId, generation))::element_type;
        return detail::WritesAtMost<Link>{webFront->findLink(webLinkId, generation), limit};
    }

    /// A sender completing on the server context after the duration; see BasicWF::resumeAfter.
    template <typename Rep, typename Period>
    [[nodiscard]] auto resumeAfter(std::chrono::duration<Rep, Period> duration) const {
        return webFront->resumeAfter(duration);
    }

    /// Lazy typed call; co_await propagates the receiver's stop token. Cancellation
    /// stops waiting locally; a JavaScript function already dispatched keeps running.
    template <typename R = void>
    [[nodiscard]] JsFunctionSender<WebFront, R> jsFunctionSender(std::string_view functionName) const {
        return JsFunctionSender<WebFront, R>{functionName, *webFront, webLinkId, generation};
    }
};

namespace detail {
template <frontend::FrontendType Frontend>
inline int ensureFrontendInitialized() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        Frontend::initialize();
    });
    return 0;
}

/// Decodes Args from the call data and invokes the callable with them, preceded by 'caller' (the
/// calling UI, for callables that take it as their first parameter, or nothing).
template <http::BuffersPolicyType Policy, typename R, typename... Args, typename Function>
auto makeCppFunctionHandler(Function&& function) {
    using StoredFunction = std::decay_t<Function>;
    auto callable = std::make_shared<StoredFunction>(std::forward<Function>(function));

    return [callable = std::move(callable)](std::span<const std::byte> data, auto&&... caller) -> R {
        std::tuple<Args...> parameters;
        auto deserializeAndCall = [&]<std::size_t... Is>(std::tuple<Args...>& tuple, std::index_sequence<Is...>) -> R {
            (msg::FunctionCall<Policy>::decodeParameter(std::get<Is>(tuple), data), ...);
            if (!data.empty())
                throw std::runtime_error("Function call contains trailing parameter data");
            if constexpr (std::is_void_v<R>) {
                std::invoke(*callable, caller..., std::get<Is>(tuple)...);
                return;
            } else
                return std::invoke(*callable, caller..., std::get<Is>(tuple)...);
        };

        return deserializeAndCall(parameters, std::index_sequence_for<Args...>());
    };
}

// --- Callable signature inference for cppFunction(name, callable), without explicit <R, Args...> ---

// Primary template intentionally has no Return/DecayedArgs members: has_callable_traits below
// treats their absence as "inference unsupported", so unmatched T (generic lambdas, functors with
// an overloaded or templated operator(), non-callables) fail detection instead of a specialization.
template <typename T>
struct callable_traits_impl {};

template <typename R, typename... Args>
struct callable_traits_impl<R (*)(Args...)> {
    using Return      = R;
    using DecayedArgs = std::tuple<std::decay_t<Args>...>;
    using RawArgs     = std::tuple<Args...>;
};
template <typename R, typename... Args>
struct callable_traits_impl<R (*)(Args...) noexcept> : callable_traits_impl<R (*)(Args...)> {};

template <typename R, typename... Args>
struct callable_traits_impl<std::function<R(Args...)>> {
    using Return      = R;
    using DecayedArgs = std::tuple<std::decay_t<Args>...>;
    using RawArgs     = std::tuple<Args...>;
};

template <typename C, typename R, typename... Args>
struct callable_traits_impl<R (C::*)(Args...)> {
    using Return      = R;
    using DecayedArgs = std::tuple<std::decay_t<Args>...>;
    using RawArgs     = std::tuple<Args...>;
};
template <typename C, typename R, typename... Args>
struct callable_traits_impl<R (C::*)(Args...) const> : callable_traits_impl<R (C::*)(Args...)> {};
template <typename C, typename R, typename... Args>
struct callable_traits_impl<R (C::*)(Args...) noexcept> : callable_traits_impl<R (C::*)(Args...)> {};
template <typename C, typename R, typename... Args>
struct callable_traits_impl<R (C::*)(Args...) const noexcept> : callable_traits_impl<R (C::*)(Args...)> {};

// A generic lambda or a functor with an overloaded operator() has no single &T::operator() (taking
// its address, or picking an overload, is ill-formed without explicit template arguments), so this
// correctly evaluates to false for exactly the callables that need explicit cppFunction<R, Args...>.
template <typename T, typename = void>
struct has_call_operator : std::false_type {};
template <typename T>
struct has_call_operator<T, std::void_t<decltype(&T::operator())>> : std::true_type {};

// Function pointers and std::function have no operator() member, so their traits apply directly;
// lambdas/functors route through their (single, non-overloaded, non-template) operator() member.
template <typename T, bool = has_call_operator<T>::value>
struct callable_traits : callable_traits_impl<T> {};
template <typename T>
struct callable_traits<T, true> : callable_traits_impl<decltype(&T::operator())> {};

template <typename T, typename = void>
struct has_callable_traits_impl : std::false_type {};
template <typename T>
struct has_callable_traits_impl<T, std::void_t<typename callable_traits<T>::Return, typename callable_traits<T>::DecayedArgs>>
    : std::true_type {};

template <typename T>
concept has_callable_traits = has_callable_traits_impl<T>::value;

/// Passes the calling UI, received as an lvalue, as the callable's declared first parameter type
/// Caller (UI, const UI&, UI& or UI&&), followed by the decoded parameters.
template <typename Caller, typename Function>
auto adaptCaller(Function&& function) {
    return [function = std::forward<Function>(function)](auto& caller, auto&&... args) mutable -> decltype(auto) {
        return std::invoke(function, static_cast<Caller>(caller), std::forward<decltype(args)>(args)...);
    };
}

/// True when a callable's first parameter is the application's UI: the calling browser is then
/// passed to it instead of being read from the call data.
template <typename UI, typename... Args>
struct takes_caller : std::false_type {};
template <typename UI, typename First, typename... Rest>
struct takes_caller<UI, First, Rest...> : std::is_same<std::remove_cvref_t<First>, UI> {};

/// The deduced parameters to register: the decayed ones, except a caller UI, which keeps its declared
/// type so that adaptCaller can pass it as a mutable or rvalue reference.
template <typename UI, typename RawArgs, typename DecayedArgs>
struct caller_aware_args {
    using type = DecayedArgs;
};
template <typename UI, typename RawFirst, typename... RawRest, typename DecayedFirst, typename... DecayedRest>
struct caller_aware_args<UI, std::tuple<RawFirst, RawRest...>, std::tuple<DecayedFirst, DecayedRest...>> {
    using type = std::conditional_t<std::is_same_v<DecayedFirst, UI>, std::tuple<RawFirst, DecayedRest...>, std::tuple<DecayedFirst, DecayedRest...>>;
};

// Bridges a deduced <Return, tuple<DecayedArgs...>> pair back to the explicit-template cppFunction
// overload, so the deduced and explicit registration paths share one dispatch/response implementation.
template <typename R, typename Tuple>
struct ApplyArgs;
template <typename R, typename... Args>
struct ApplyArgs<R, std::tuple<Args...>> {
    template <typename Self, typename Function>
    static bool invoke(Self& self, std::string functionName, Function&& function) {
        return self.template cppFunction<R, Args...>(std::move(functionName), std::forward<Function>(function));
    }
};

/// A self-contained return message carrying only 'error'.
template <typename Net, http::BuffersPolicyType Policy>
websocket::Frame<Net> makeFailureFrame(msg::CallId callId, const std::exception& error) {
    msg::FunctionReturn<Policy> failure;
    failure.setCallId(callId);
    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(failure.header().data()), failure.header().size())};
    failure.encodeParameter(error, frame);
    frame.freeze();
    return frame;
}

template<typename Net, http::BuffersPolicyType Policy, typename R, typename Callable>
auto makeCppFunctionResponder(Callable&& callable) {
    return [callable = std::forward<Callable>(callable)](std::span<const std::byte> data, auto& link, msg::CallId callId, auto&&... caller) mutable {
        msg::FunctionReturn<Policy> result;
        result.setCallId(callId);
        websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(result.header().data()), result.header().size())};
        // A failure may come from the callback or from encoding its value (a string over the wire
        // limit), which can leave 'result' partly encoded: failures are sent in a fresh message.
        try {
            if constexpr (std::is_void_v<R>)
                callable(data, caller...);
            else {
                auto value = callable(data, caller...);
                result.encodeParameter(value, frame);
                frame.freeze();
            }
        } catch (const std::exception& error) {
            frame = makeFailureFrame<Net, Policy>(callId, error);
        } catch (...) {
            frame = makeFailureFrame<Net, Policy>(callId, std::runtime_error{"Unknown C++ exception"});
        }
        if (callId != 0)
            link.sendFrame(std::move(frame));
    };
}
}  // namespace detail

/**
 * @brief Configuration for the newcomer-friendly default construction of BasicWF.
 *
 * An empty documentRoot means no local document root is configured: the default filesystem
 * (webfront::fs::DefaultFS) then performs no local filesystem access at all and serves only its
 * embedded assets. Setting documentRoot enables serving local files for development; see
 * webfront::fs::DefaultFS for the precedence between the embedded bridge script, the configured
 * root, and the embedded fallback page.
 */
struct WebFrontConfig {
    std::string           address{"127.0.0.1"};
    /// "0" requests an ephemeral port with TCPStdnet. The frontend receives the bound port.
    std::string           port{"9002"};
    std::filesystem::path documentRoot{};
    ContentPolicy         contentPolicy{};
    /// Initial window of the embedded frontends; the title defaults to the executable's name.
    WindowOptions         window{};
};

template <typename NetProvider, typename Filesystem, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy,
          frontend::FrontendType Frontend = frontend::DefaultFrontend>
class BasicWF {
public:
    using Net              = NetProvider;
    using BufferPolicy     = Policy;
    using FrontendProvider = Frontend;
    using UI               = BasicUI<BasicWF<Net, Filesystem, Policy, Frontend>>;

    // Newcomer-friendly defaults: loopback address, port 9002, no local document root.
    BasicWF() : BasicWF(WebFrontConfig{}) {}

    explicit BasicWF(WebFrontConfig config)
        : httpServer((detail::ensureFrontendInitialized<Frontend>(), config.address), config.port, config.documentRoot, config.contentPolicy),
          httpAddress(config.address),
          httpPort(httpServer.port()),
          httpDocRoot(config.documentRoot),
          contentPolicy(std::move(config.contentPolicy)),
          windowOptions(std::move(config.window)),
          idsCounter(0) {
        // Links are created on the server thread, outside the lock: identifiers are only assigned here.
        httpServer.onUpgrade([this](typename Net::Socket&& socket) {
            WebLinkId         id{};
            WebLinkGeneration generation{};
            {
                std::scoped_lock lock{linksMutex};
                while (webLinks.contains(idsCounter))
                    ++idsCounter;
                id         = idsCounter++;
                generation = ++generationCounter;
            }
            auto link = std::make_shared<WebLink<Net, Policy>>(std::move(socket), id, [this](WebLinkEvent event) {
                onEvent(event);
            }, httpServer.taskScope(), false);
            {
                std::scoped_lock lock{linksMutex};
                webLinks.emplace(id, Connection{.link = link, .generation = generation, .thread = std::this_thread::get_id(), .closed = false, .stage = Connection::Stage::handshaking, .deferred = {}});
            }
            link->start();  // registered first: every event it dispatches finds it
        });
    }

    // Preserved for source compatibility; binds all interfaces like it always has, unlike the
    // loopback-by-default WebFrontConfig constructor above.
    explicit BasicWF(std::string_view port, std::filesystem::path docRoot = ".")
        : BasicWF(WebFrontConfig{.address = "0.0.0.0", .port = std::string(port), .documentRoot = std::move(docRoot)}) {}

    // Each UI and the server callbacks hold the application's address: it never moves.
    BasicWF(const BasicWF&)            = delete;
    BasicWF& operator=(const BasicWF&) = delete;

    /** @brief The listening port, including the OS-assigned TCPStdnet port when configured with "0". */
    std::string_view port() const noexcept { return httpPort; }

    ~BasicWF() noexcept {
        stopAndJoinServer();
        // Coroutine cleanup may use its UI, so links stay alive through the join.
        httpServer.drainUntilJoined();
        releaseLinks();
    }

    /** @brief Stops and joins owned work before returning. Call outside server callbacks,
     * after any external callers of run()/runOne() have returned. Safe to repeat.
     */
    void shutdown() {
        stopAndJoinServer();
        // Application coroutine cleanup may still use its UI: retain links until join.
        httpServer.drain();
        releaseLinks();
    }

    /** @brief Thread-safe while the application lives. Starts on the server thread.
     * Rejected calls return a stopped handle without starting the body.
     * Accepted tasks can start before shutdown cancels and drains them.
     * Ignoring the returned handle keeps the task running until completion or shutdown.
     */
    TaskHandle spawn(Task task) {
        return httpServer.startTask(std::move(task));
    }

    /** @brief A sender completing on the server context once the duration has elapsed. co_await propagates the
     * task's stop token: a stop request (TaskHandle::requestStop, shutdown) completes it stopped at once.
     */
    template <typename Rep, typename Period>
    [[nodiscard]] auto resumeAfter(std::chrono::duration<Rep, Period> duration) {
        return httpServer.resumeAfter(duration);
    }

    void run() {
        httpServer.run();
    }
    void runOne() {
        httpServer.runOne();
    }

    void stop() {
        httpServer.stop();
    }

    /// Sets the application's handler for each connected browser, replacing any previous one.
    void onUIStarted(std::function<void(UI)>&& handler) {
        uiStartedHandler = std::move(handler);
    }

    /// Adds a handler for each browser that connects from now on, called after the onUIStarted
    /// handler and the earlier subscribers. It stays registered while the returned subscription
    /// lives; components use this so they never replace the application's handler.
    [[nodiscard]] UISubscription subscribeUIStarted(std::function<void(UI)> handler) {
        return uiSubscribers->subscribe(std::move(handler));
    }

    /// Adds a handler called on the server thread when a browser closes its connection, or the connection fails
    /// (a failed write included), while the returned subscription lives. Its UI no longer reaches the browser: the
    /// connection is released after the handlers, then calls to it are dropped and typed calls fail. Application
    /// shutdown, which stops every connection itself, notifies no handler.
    [[nodiscard]] UISubscription subscribeUIClosed(std::function<void(UI)> handler) {
        return uiClosedSubscribers->subscribe(std::move(handler));
    }
    /// The link of a connected browser, shared with the caller so that it stays valid while used, from any thread;
    /// null once its connection closed and was released, or when `id` now belongs to a later connection (another generation).
    [[nodiscard]] std::shared_ptr<WebLink<Net, Policy>> findLink(WebLinkId id, WebLinkGeneration generation) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        return found == webLinks.end() || found->second.generation != generation ? nullptr : found->second.link;
    }

private:
    template <typename>
    friend class BasicUI;
    template <typename>
    friend class Broadcast;
    template <typename>
    friend struct testing::ConnectionIds;  // lets a test start the identifiers near their wrap

    /// Sends `call` to each browser not reported closed whose onUIStarted handlers have returned: at once, on the
    /// calling thread and outside every lock. For a browser whose handlers are running, `call` is kept, within its
    /// limit, and sent on the server thread as soon as they return, after what they sent and in the order of the calls.
    /// Returns once `call` is queued or kept for every browser: a call that follows it (same thread, or synchronized
    /// after it) comes after it. Overlapping calls are not ordered: holding linksMutex while writing would not order
    /// them either, and would hold a WebFront lock while the write starts.
    void sendToStartedLinks(const detail::BroadcastCall<Net>& call) {
        std::vector<std::shared_ptr<WebLink<Net, Policy>>> started;
        {
            std::scoped_lock lock{linksMutex};
            started.reserve(webLinks.size());
            for (auto& [id, connection] : webLinks) {
                if (connection.closed)
                    continue;
                if (connection.stage == Connection::Stage::started)
                    started.push_back(connection.link);
                else if (connection.stage == Connection::Stage::starting)
                    call.deferInto(connection.deferred);
            }
        }
        // A browser whose send fails must not deprive the next ones of the call.
        for (const auto& link : started) {
            try {
                call.sendTo(*link);
            } catch (const std::exception& error) {
                log::error("A broadcast to a browser failed: {}", error.what());
            }
        }
    }

    /// The generation of the connection that now owns `id`: 0, which no connection has, when none does.
    WebLinkGeneration generationOf(WebLinkId id) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        return found == webLinks.end() ? WebLinkGeneration{0} : found->second.generation;
    }

    /// The link and generation of the connection that now owns `id`, read under one lock (null and 0 when none does).
    std::pair<std::shared_ptr<WebLink<Net, Policy>>, WebLinkGeneration> linkAndGeneration(WebLinkId id) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        if (found == webLinks.end())
            return {nullptr, WebLinkGeneration{0}};
        return {found->second.link, found->second.generation};
    }

    /// The UI of the connection that now owns `id` (generation 0, reaching nothing, when none does).
    UI uiOf(WebLinkId id) {
        return UI{*this, id, generationOf(id)};
    }

    /// Internal DOM document storage, confined to the server thread.
    std::shared_ptr<dom::detail::State> domState(WebLinkId id, WebLinkGeneration generation) {
        std::shared_ptr<WebLink<Net, Policy>> link;
        {
            std::scoped_lock lock{linksMutex};
            const auto       found = webLinks.find(id);
            if (found == webLinks.end() || found->second.generation != generation || found->second.closed)
                throw ConnectionError("Connection with client lost");
            if (found->second.thread != std::this_thread::get_id())
                throw std::logic_error("DOM document requires the server thread");
            link = found->second.link;
        }
        // The connection's immutable server-thread identity was checked before touching this map.
        if (auto found = domDocuments.find(id); found != domDocuments.end())
            return found->second;
        auto state  = std::make_shared<dom::detail::State>();
        state->send = [weak = std::weak_ptr(link)](const msg::DomBatch& batch) {
            const auto connection = weak.lock();
            if (!connection)
                throw ConnectionError("Connection with client lost");
            connection->sendCommand(batch);
        };
        state->schedule = [this, id, generation, weak = std::weak_ptr(state)] {
            if (auto shared = weak.lock())
                static_cast<void>(spawn(flushDomAfter(UI{*this, id, generation}, std::move(shared))));
        };
        // Called under the document's release lock, which close() takes before the application releases it.
        state->post = [this, weak = std::weak_ptr(state)] {
            static_cast<void>(spawn(releaseDomWaits(weak)));
        };
        domDocuments.emplace(id, state);
        return state;
    }

public:
    /// The browsers connected now: a closed connection is released shortly after its closure. Counts connections still
    /// in their handshake or onUIStarted handlers too: not the number of browsers a broadcast or forEachUI reaches.
    [[nodiscard]] std::size_t connectedBrowsers() const {
        std::scoped_lock lock{linksMutex};
        return webLinks.size();
    }

    /// Calls `function(ui)` for each browser not reported closed whose onUIStarted handlers had returned when the call
    /// started; a browser whose handlers are running is not visited. Callable from any thread; `function` runs on the
    /// calling thread, outside every WebFront lock, so it may call the UI, but ui.document() needs the server thread.
    /// A starting browser that is skipped misses the call: send states that every browser must follow with broadcast().
    template <std::invocable<UI> Function>
    void forEachUI(Function&& function) {
        std::vector<UI> started;
        {
            std::scoped_lock lock{linksMutex};
            for (const auto& [id, connection] : webLinks)
                if (!connection.closed && connection.stage == Connection::Stage::started)
                    started.emplace_back(*this, id, connection.generation);
        }
        for (const auto& ui : started)
            std::invoke(function, ui);
    }

    /// A function object calling `functionName` on every started browser, fire-and-forget, from any thread; see
    /// BroadcastOptions for slow browsers.
    [[nodiscard]] Broadcast<BasicWF> broadcast(std::string_view functionName, BroadcastOptions options = {}) {
        return Broadcast<BasicWF>{functionName, *this, options};
    }

    /// Whether a C++ function of that name is registered.
    [[nodiscard]] bool hasCppFunction(std::string_view functionName) const {
        return cppFunctions.contains(functionName);
    }

    /**
     * @brief Registers a function which will be callable from Javascript.
     *
     * When the first of Args is UI, the callable receives the browser that called it there, and only
     * the remaining parameters come from JavaScript.
     *
     * @tparam R ReturnType of the CppFunction
     * @tparam Args parameters of the CppFunction
     * @param functionName
     * @param function
     * @return false when a function of that name was already registered, which stays registered
     */
    template <typename R, typename... Args>
    bool cppFunction(std::string functionName, auto&& function) {
        if constexpr (detail::takes_caller<UI, Args...>::value)
            return registerWithCaller<R, Args...>(std::move(functionName), std::forward<decltype(function)>(function));
        else {
            auto callable  = detail::makeCppFunctionHandler<Policy, R, Args...>(std::forward<decltype(function)>(function));
            auto responder = detail::makeCppFunctionResponder<Net, Policy, R>(std::move(callable));
            return cppFunctions
                .try_emplace(std::move(functionName),
                             [responder](std::span<const std::byte> data, WebLink<Net, Policy>& link, msg::CallId callId, UI) mutable {
                                 responder(data, link, callId);
                             })
                .second;
        }
    }

    /**
     * @brief Registers a function which will be callable from Javascript, deducing its return type
     * and parameter types from the callable itself.
     *
     * Supports ordinary and mutable lambdas, function pointers, and std::function. Generic lambdas,
     * functors with an overloaded operator(), and other callables that don't have a single concrete
     * operator() cannot be deduced this way; use the explicit cppFunction<ReturnType, ArgTypes...>
     * overload for those.
     *
     * @param functionName
     * @param function
     * @return false when a function of that name was already registered, which stays registered
     */
    template <typename Function>
    bool cppFunction(std::string functionName, Function&& function) {
        using Callable = std::remove_cvref_t<Function>;
        static_assert(detail::has_callable_traits<Callable>,
                      "webfront::BasicWF::cppFunction: could not deduce this callable's signature. Generic "
                      "lambdas, overloaded operator(), and other advanced callables require explicit "
                      "cppFunction<ReturnType, ArgTypes...>(name, callable).");
        using Traits = detail::callable_traits<Callable>;
        using Args   = typename detail::caller_aware_args<UI, typename Traits::RawArgs, typename Traits::DecayedArgs>::type;
        return detail::ApplyArgs<typename Traits::Return, Args>::invoke(*this, std::move(functionName), std::forward<Function>(function));
    }

    enum class WindowAction { none, closeWindow };
#ifdef _MSC_VER
    // A Frontend::open() that unconditionally throws (e.g. CEFFrontend::open() when CEF support
    // isn't compiled in) makes the returns below provably unreachable for that instantiation, both
    // here and in openAndRun() below it (which calls openWindow()). MSVC detects this via inlining
    // and, uniquely among our compilers, flags it as an error under /WX; it's expected, not a bug,
    // so it's suppressed narrowly rather than relaxed project-wide.
    #pragma warning(push)
    #pragma warning(disable : 4702)
#endif
    WindowAction openWindow(std::string_view htmlFilename) {
        if constexpr (requires { Frontend::open(httpAddress, httpPort, htmlFilename, contentPolicy, windowOptions); })
            Frontend::open(httpAddress, httpPort, htmlFilename, contentPolicy, windowOptions);
        else if constexpr (requires { Frontend::open(httpAddress, httpPort, htmlFilename, contentPolicy); })
            Frontend::open(httpAddress, httpPort, htmlFilename, contentPolicy);
        else if constexpr (requires { Frontend::open(httpAddress, httpPort, htmlFilename); })
            Frontend::open(httpAddress, httpPort, htmlFilename);
        else
            Frontend::open(httpPort, htmlFilename);
        if constexpr (requires { Frontend::closeServerAfterOpen(); })
            return Frontend::closeServerAfterOpen() ? WindowAction::closeWindow : WindowAction::none;
        else if constexpr (Frontend::action == frontend::Action::closeServerAfterOpen)
            return WindowAction::closeWindow;
        else
            return WindowAction::none;
    }

    // Starts the server in a background thread and opens the frontend. An embedded window's close
    // or a frontend exception stops and joins the server. An external browser keeps it running.
    void openAndRun(std::string_view htmlFilename) {
        // The constructor has already bound and listened; this thread services the queued I/O.
        // Launch server only once or restart if previous thread ended
        if (!serverThread.joinable()) {
            serverThread = std::thread([this] {
                run();
            });
        }
        WindowAction action;
        try {
            action = openWindow(htmlFilename);
        } catch (...) {
            stop();
            if (serverThread.joinable())
                serverThread.join();
            throw;
        }
        if (action == WindowAction::closeWindow) {
            stop();
            if (serverThread.joinable())
                serverThread.join();
        }
    }
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

    // Starts the server and opens the page on the calling thread. Embedded frontends return after
    // window closure and server shutdown; the system browser waits for Enter and leaves the server running.
    void show(std::string_view page = "index.html") {
        openAndRun(page);
    }

private:
    void stopAndJoinServer() {
        stop();
        if (serverThread.joinable())
            serverThread.join();
    }

    // A released link is destroyed outside the lock, by its last owner: a call on another thread may still use it.
    Task releaseLink(WebLinkId id) {
        std::shared_ptr<WebLink<Net, Policy>> released;
        {
            std::scoped_lock lock{linksMutex};
            if (auto found = webLinks.find(id); found != webLinks.end()) {
                released = std::move(found->second.link);
                webLinks.erase(found);
            }
        }
        co_return;
    }

    void releaseLinks() {
        for (auto& [id, document] : domDocuments)
            document->close();
        domDocuments.clear();
        std::map<WebLinkId, Connection>                           released;
        std::scoped_lock                                          lock{linksMutex};
        released.swap(webLinks);
    }

    template <typename R, typename Caller, typename... Args>
    bool registerWithCaller(std::string functionName, auto&& function) {
        auto adapted   = detail::adaptCaller<Caller>(std::forward<decltype(function)>(function));
        auto callable  = detail::makeCppFunctionHandler<Policy, R, Args...>(std::move(adapted));
        auto responder = detail::makeCppFunctionResponder<Net, Policy, R>(std::move(callable));
        return cppFunctions
            .try_emplace(std::move(functionName),
                         [responder](std::span<const std::byte> data, WebLink<Net, Policy>& link, msg::CallId callId, UI caller) mutable {
                             responder(data, link, callId, caller);
                         })
            .second;
    }

    http::Server<Net, Filesystem, Policy>                                    httpServer;
    std::string                                                              httpAddress;
    std::string                                                              httpPort;
    std::filesystem::path                                                    httpDocRoot;
    ContentPolicy                                                            contentPolicy;
    WindowOptions                                                            windowOptions;
    mutable std::mutex                                                       linksMutex;  // guards webLinks, read from any thread
    struct Connection {
        std::shared_ptr<WebLink<Net, Policy>> link;
        WebLinkGeneration                     generation;
        std::thread::id                       thread;
        bool                                  closed;
        enum class Stage { handshaking, starting, started };
        Stage                                  stage = Stage::handshaking;  // starting while its onUIStarted handlers run
        std::vector<detail::BroadcastCall<Net>> deferred;                    // sendToStartedLinks() calls while starting
    };
    std::map<WebLinkId, Connection>                                          webLinks;
    WebLinkId                                                              idsCounter{0};
    WebLinkGeneration                                                      generationCounter{0};  // guarded by linksMutex; the first connection is 1
    std::function<void(UI)>                                                uiStartedHandler;
    std::shared_ptr<detail::UISubscribers<UI>>                               uiSubscribers = std::make_shared<detail::UISubscribers<UI>>();
    std::shared_ptr<detail::UISubscribers<UI>>                               uiClosedSubscribers = std::make_shared<detail::UISubscribers<UI>>();
    std::map<std::string, std::function<void(std::span<const std::byte>, WebLink<Net, Policy>&, msg::CallId, UI)>, std::less<>> cppFunctions;
    // Keyed by identifier alone: a document is closed and erased on its connection's closed event, before the link is
    // released and its identifier can be reused, so two connections never share an entry.
    std::map<WebLinkId, std::shared_ptr<dom::detail::State>>                                                                    domDocuments;
    std::thread                                                            serverThread;  // Background thread running the HTTP server

private:
    static Task flushDomAfter(UI ui, std::shared_ptr<dom::detail::State> state) {
        co_await ui.resumeAfter(std::chrono::milliseconds(1));
        state->scheduled = false;
        if (!state->alive)
            co_return;
        try {
            state->flush();
        } catch (const std::exception& error) {
            state->reject(state->batchId, error.what());
        }
    }

    static Task releaseDomWaits(std::weak_ptr<dom::detail::State> weak) {
        if (auto state = weak.lock(); state && state->alive) {
            try {
                state->sweep();
            } catch (const std::exception& error) {
                state->reject(state->batchId, error.what());
            }
        }
        co_return;
    }

    bool domCall(const WebLinkEvent& event, WebLink<Net, Policy>& link) {
        if (event.text == "wfDom.ready") {
            auto handler = detail::makeCppFunctionHandler<Policy, void>([this, id = event.webLinkId, generation = generationOf(event.webLinkId)] {
                auto state = domState(id, generation);
                state->check();
                if (state->ready)
                    return;  // Retrying a lost readiness acknowledgement must not reset the document.
                state->ready = true;
                state->wake();
            });
            detail::makeCppFunctionResponder<Net, Policy, void>(std::move(handler))(event.data, link, event.callId);
            return true;
        }
        const auto found = domDocuments.find(event.webLinkId);
        if (found == domDocuments.end() || !found->second->ready)
            return false;  // Low-level batch users retain their own callbacks until explicit connection.
        auto state = found->second;
        if (event.text == "wfDom.event") {
            auto handler = detail::makeCppFunctionHandler<Policy, void, std::uint32_t, std::uint32_t, std::string>(
                [state](std::uint32_t node, std::uint32_t name, std::string value) {
                    if (state->alive)
                        state->event(node, name, std::move(value));
                });
            detail::makeCppFunctionResponder<Net, Policy, void>(std::move(handler))(event.data, link, event.callId);
        } else if (event.text == "wfDom.revoked") {
            auto handler = detail::makeCppFunctionHandler<Policy, void, std::string>([state](std::string name) {
                if (state->alive)
                    state->revoke(name);
            });
            detail::makeCppFunctionResponder<Net, Policy, void>(std::move(handler))(event.data, link, event.callId);
        } else if (event.text == "wfDom.rejected") {
            auto handler = detail::makeCppFunctionHandler<Policy, void, std::uint32_t, std::string>([state](std::uint32_t id, std::string reason) {
                if (state->alive)
                    state->reject(id, std::move(reason));
            });
            detail::makeCppFunctionResponder<Net, Policy, void>(std::move(handler))(event.data, link, event.callId);
        } else
            return false;
        return true;
    }

    // After the onUIStarted handlers: the sends deferred meanwhile run in order, those arriving while they run too,
    // and only then do new sends reach the browser directly.
    void finishStarting(WebLinkId id) noexcept {
        for (;;) {
            std::vector<detail::BroadcastCall<Net>> deferred;
            std::shared_ptr<WebLink<Net, Policy>>   link;
            {
                std::scoped_lock lock{linksMutex};
                const auto       found = webLinks.find(id);
                if (found == webLinks.end())
                    return;
                if (found->second.deferred.empty() || found->second.closed) {
                    found->second.deferred.clear();
                    found->second.stage = Connection::Stage::started;
                    return;
                }
                deferred.swap(found->second.deferred);
                link = found->second.link;
            }
            for (const auto& call : deferred) {
                try {
                    call.sendTo(*link);
                } catch (const std::exception& error) {
                    log::error("A broadcast to a starting browser failed: {}", error.what());
                }
            }
        }
    }

    void onEvent(WebLinkEvent event) {
        switch (event.code) {
            case WebLinkEvent::Code::linked: {
                {
                    std::scoped_lock lock{linksMutex};
                    if (auto found = webLinks.find(event.webLinkId); found != webLinks.end())
                        found->second.stage = Connection::Stage::starting;
                }
                // Even when a handler throws: the browser must not keep collecting deferred sends.
                struct Started {
                    BasicWF*  app;
                    WebLinkId id;
                    ~Started() {
                        app->finishStarting(id);
                    }
                } started{this, event.webLinkId};
                if (uiStartedHandler)
                    uiStartedHandler(uiOf(event.webLinkId));
                uiSubscribers->notify(uiOf(event.webLinkId));
                break;
            }
            case WebLinkEvent::Code::closed:
                // Shutdown cancels the connections' writes, which then fail and report a closure: not a disconnection.
                if (httpServer.taskScope().stopRequested())
                    break;
                {
                    std::scoped_lock lock{linksMutex};
                    if (auto found = webLinks.find(event.webLinkId); found != webLinks.end()) {
                        found->second.closed = true;
                        found->second.deferred.clear();
                    }
                }
                if (auto found = domDocuments.find(event.webLinkId); found != domDocuments.end()) {
                    found->second->close();
                    domDocuments.erase(found);
                }
                uiClosedSubscribers->notify(uiOf(event.webLinkId));
                // The link is running this handler: it is released afterwards, by a task on the server context.
                // During shutdown the task is refused and the link goes with the application.
                static_cast<void>(httpServer.startTask(releaseLink(event.webLinkId)));
                break;
            case WebLinkEvent::Code::cppFunctionCalled: {
                // Links are registered before they read, and released after their closure: the caller is registered.
                const auto [link, generation] = linkAndGeneration(event.webLinkId);
                if (!link)
                    break;
                if constexpr (requires(typename Net::Acceptor& acceptor) { Net::ResumeAfter(acceptor, std::chrono::milliseconds(1)); }) {
                    if (domCall(event, *link))
                        break;
                } else if (event.text == "wfDom.ready") {
                    link->sendError(event.callId, "DOM API requires Net::ResumeAfter");
                    break;
                }
                if (auto function = cppFunctions.find(event.text); function != cppFunctions.end())
                    function->second(event.data, *link, event.callId, UI{*this, event.webLinkId, generation});
                else
                    link->sendError(event.callId, "C++ function '" + event.text + "' was not found");
                break;
            }
        }
    }
};

template <typename NetProvider, typename Filesystem, frontend::FrontendType Frontend, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
using BasicWFWithFrontend = BasicWF<NetProvider, Filesystem, Policy, Frontend>;

using WebFront = BasicWF<NetProvider, fs::DefaultFS>;
template <frontend::FrontendType Frontend, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
using WebFrontWithFrontend = BasicWF<NetProvider, fs::DefaultFS, Policy, Frontend>;
using UI       = WebFront::UI;

}  // namespace webfront

// Optional templates instantiate only when the developer/native facade is selected.
#include "DeveloperApp.hpp"
