// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-01-18
//
// WebFront — core
// Role: Main orchestration API: BasicWF owns the HTTP server, the connected browsers, the registered callbacks and the server thread.
// Thread: ui-loop | Alloc: unrestricted
// LLM: public API; read AGENTS.md invariants and the class contracts before the implementation

#pragma once

#include "BridgeError.hpp"
#include "Broadcast.hpp"
#include "DeferredCalls.hpp"
#include "details/SpaceState.hpp"
#include "dom/Document.hpp"
#include "frontend/Frontend.hpp"
#include "http/HTTPServer.hpp"
#include "Introspection.hpp"
#include "JsFunction.hpp"
#include "JsFunctionSender.hpp"
#include "networking/TCPNetworkingStdnet.hpp"
#include "ReadBinding.hpp"
#include "system/DefaultFS.hpp"
#include "system/IndexFS.hpp"
#include "system/WindowsCompat.hpp"
#include "Task.hpp"
#include "UISubscribers.hpp"
#include "ViewAssociation.hpp"
#include "Watches.hpp"
#include "weblink/DomBatch.hpp"
#include "weblink/Messages.hpp"
#include "weblink/WebLink.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
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

namespace pty::detail {
struct PageRegistration;
}  // namespace pty::detail

template <typename App>
class BasicSpace;

inline constexpr std::string_view version = "1.2.0";

namespace testing {
/// Test-only access to the connection identifiers of an application; not part of the API.
template <typename Application>
struct ConnectionIds;
/// Test-only access to the launch secret of an application; not part of the API.
template <typename Application>
struct LaunchSecret;
}  // namespace testing

using NetProvider     = networking::TCPStdnet;
using ConnectionError = std::runtime_error;

template <typename WebFront>
class BasicUI {
    WebFront*         webFront;  ///< Never null: the only constructor takes a reference, and there is no default constructor.
    WebLinkId         webLinkId;
    WebLinkGeneration generation;

public:
    BasicUI(WebFront& wf, WebLinkId id, WebLinkGeneration connectionGeneration) : webFront(&wf), webLinkId(id), generation(connectionGeneration) { }

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
        if (!link)
            throw ConnectionError("Connection with client lost");
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

    /// True when this connection proved, with the secret of this launch, that it comes from the view of an embedded
    /// frontend of this application (CEF, WebView2, WKWebView, WebKitGTK). The system browser never is: no secret is
    /// given to it, whatever its address. False once the connection is gone.
    [[nodiscard]] bool isEmbeddedView() const {
        return webFront->isEmbeddedView(webLinkId, generation);
    }

    /// The native window this connection's view lives in, for a native dialog's parent (ADR-020). Only the view of an
    /// embedded frontend has one: any other connection (the system browser, a remote client, a loopback peer) gets
    /// `notEmbedded`, so a dialog never attaches to a window that is not the caller's. Otherwise the answers and the
    /// thread rule of frontend::NativeWindow::get() apply: call it on the interface thread (BasicWF::uiExecutor()).
    [[nodiscard]] frontend::NativeWindowResult nativeWindow() const {
        return webFront->nativeWindowOf(webLinkId, generation);
    }

    /// The only criterion of a "local client" for system services (ADR-020, rule 3): an embedded view, or a
    /// loopback peer when the application set WebFrontConfig::trustLoopbackClients. The address is otherwise never
    /// consulted. False once the connection is gone.
    [[nodiscard]] bool isLocalClient() const {
        return webFront->isLocalClient(webLinkId, generation);
    }

    /// The path on which this browser opened its bridge, decoded and normalized as a file request reads it (a '+' stays a
    /// '+', "%20" is a space), without the query: by default the directory of its page, "/" for a page at the root, "/a/b/" for `/a/b/index.html`. Under a
    /// space, the path within it: "/" for a page of `/s/abc/`, "/sub/" for one of `/s/abc/sub/`. The client chooses it,
    /// so it locates the page and proves nothing. Still known to the closed handlers; empty once the connection is released.
    [[nodiscard]] std::string path() const {
        return webFront->pathOf(webLinkId, generation);
    }

    /// A read URL on `route` for this browser's connection: the page reads it with `<img>`, `<video>`, `fetch` or a
    /// download; `path` is what the route's handler receives (ReadGrant::rest). Unless `options.transferable`, the URL
    /// works only from the browser client of this connection and only while the connection is open; it stays valid for
    /// repeated requests (byte ranges). Throws ReadRefused when the connection is gone, when it is not the view of an
    /// embedded frontend and WebFrontConfig::remoteFileTransport was not declared, or when the options are unacceptable.
    [[nodiscard]] std::string grantRead(const ReadRoute& route, std::string_view path, ReadOptions options = {}) const {
        // A generation is only unique within its application: a route of another one would own the wrong connection.
        if (route.binding().get() != webFront->readBinding.get())
            throw ReadRefused("The read route belongs to another application");
        return route.binding()->grant(generation, route.id(), route.prefix(), path, std::move(options));
    }

    /// Lazy typed call; co_await propagates the receiver's stop token. Cancellation
    /// stops waiting locally; a JavaScript function already dispatched keeps running.
    template <typename R = void>
    [[nodiscard]] JsFunctionSender<WebFront, R> jsFunctionSender(std::string_view functionName) const {
        return JsFunctionSender<WebFront, R>{functionName, *webFront, webLinkId, generation};
    }
};

namespace detail {
/// The application that currently shows a window through `Frontend`, process-wide like the frontend itself: only one
/// show() at a time can own it, so a second application's request never reaches the first's window.
template <typename Frontend>
struct FrontendSessionOwner {
    static inline std::atomic<const void*> owner{nullptr};
};

/// True for the loopback address of either family, including an IPv4 loopback seen through a dual-stack socket (::ffff:127.0.0.1).
template <typename Address>
[[nodiscard]] bool isLoopbackAddress(const Address& address) {
    if (address.is_loopback())
        return true;
    if constexpr (requires {
                      address.is_v6();
                      address.to_v6().is_v4_mapped();
                      address.to_v6().to_bytes();
                  }) {
        if (address.is_v6()) {
            const auto v6 = address.to_v6();
            return v6.is_v4_mapped() && v6.to_bytes()[12] == 127;
        }
    }
    return false;
}

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
    auto callable        = std::make_shared<StoredFunction>(std::forward<Function>(function));

    return [callable = std::move(callable)](std::span<const std::byte> data, auto&&... caller) -> R {
        std::tuple<Args...> parameters;
        auto                deserializeAndCall = [&]<std::size_t... Is>(std::tuple<Args...>& tuple, std::index_sequence<Is...>) -> R {
            // Arguments that cannot be decoded as the callback declared them reject with the `webfront.malformed` code.
            try {
                (msg::FunctionCall<Policy>::decodeParameter(std::get<Is>(tuple), data), ...);
                if (!data.empty())
                    throw std::runtime_error("Function call contains trailing parameter data");
            } catch (const std::exception& error) {
                throw detail::makeBridgeError(bridge_code::malformed, error.what());
            }
            if constexpr (std::is_void_v<R>) {
                std::invoke(*callable, caller..., std::get<Is>(tuple)...);
                return;
            } else
                return std::invoke(*callable, caller..., std::get<Is>(tuple)...);
        };

        return deserializeAndCall(parameters, std::index_sequence_for<Args...>());
    };
}

/// Like makeCppFunctionHandler for a callback that answers later: it returns the sender of the callback, which owns the decoded
/// arguments and the calling UI (they live in its operation state). A callback that takes them by reference, a coroutine for instance, can thus
/// use them until it completes, long after this handler returned.
template <http::BuffersPolicyType Policy, typename... Args, typename Function>
auto makeDeferredHandler(Function&& function) {
    using StoredFunction = std::decay_t<Function>;
    auto callable        = std::make_shared<StoredFunction>(std::forward<Function>(function));

    return [callable = std::move(callable)](std::span<const std::byte> data, auto&&... caller) {
        // The caller (the calling UI) is kept with the arguments, as a copy: a callback taking it by reference lives as long as them.
        using State = std::pair<std::tuple<std::decay_t<decltype(caller)>...>, std::tuple<Args...>>;
        auto state  = std::make_shared<State>(std::tuple<std::decay_t<decltype(caller)>...>{caller...}, std::tuple<Args...>{});
        try {
            std::apply(
                [&](auto&... decoded) {
                    (msg::FunctionCall<Policy>::decodeParameter(decoded, data), ...);
                },
                state->second);
            if (!data.empty())
                throw std::runtime_error("Function call contains trailing parameter data");
        } catch (const std::exception& error) {
            throw detail::makeBridgeError(bridge_code::malformed, error.what());
        }
        auto sender = std::apply(
            [&](auto&... kept) {
                return std::apply(
                    [&](auto&... decoded) {
                        return std::invoke(*callable, kept..., decoded...);
                    },
                    state->second);
            },
            state->first);
        // The callable is kept with the state until the sender completes: its coroutine refers to the callable's captures,
        // which must outlive a registration released meanwhile (a space destroyed while the call runs).
        return networking::ex::let_value(networking::ex::just(std::move(state), callable), [sender = std::move(sender)](auto&, auto&) mutable {
            return std::move(sender);
        });
    };
}

// --- Callable signature inference for cppFunction(name, callable), without explicit <R, Args...> ---

// Primary template intentionally has no Return/DecayedArgs members: has_callable_traits below
// treats their absence as "inference unsupported", so unmatched T (generic lambdas, functors with
// an overloaded or templated operator(), non-callables) fail detection instead of a specialization.
template <typename T>
struct callable_traits_impl { };

template <typename R, typename... Args>
struct callable_traits_impl<R (*)(Args...)> {
    using Return      = R;
    using DecayedArgs = std::tuple<std::decay_t<Args>...>;
    using RawArgs     = std::tuple<Args...>;
};
template <typename R, typename... Args>
struct callable_traits_impl<R (*)(Args...) noexcept> : callable_traits_impl<R (*)(Args...)> { };

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
struct callable_traits_impl<R (C::*)(Args...) const> : callable_traits_impl<R (C::*)(Args...)> { };
template <typename C, typename R, typename... Args>
struct callable_traits_impl<R (C::*)(Args...) noexcept> : callable_traits_impl<R (C::*)(Args...)> { };
template <typename C, typename R, typename... Args>
struct callable_traits_impl<R (C::*)(Args...) const noexcept> : callable_traits_impl<R (C::*)(Args...)> { };

// A generic lambda or a functor with an overloaded operator() has no single &T::operator() (taking
// its address, or picking an overload, is ill-formed without explicit template arguments), so this
// correctly evaluates to false for exactly the callables that need explicit cppFunction<R, Args...>.
template <typename T, typename = void>
struct has_call_operator : std::false_type { };
template <typename T>
struct has_call_operator<T, std::void_t<decltype(&T::operator())>> : std::true_type { };

// Function pointers and std::function have no operator() member, so their traits apply directly;
// lambdas/functors route through their (single, non-overloaded, non-template) operator() member.
template <typename T, bool = has_call_operator<T>::value>
struct callable_traits : callable_traits_impl<T> { };
template <typename T>
struct callable_traits<T, true> : callable_traits_impl<decltype(&T::operator())> { };

template <typename T, typename = void>
struct has_callable_traits_impl : std::false_type { };
template <typename T>
struct has_callable_traits_impl<T, std::void_t<typename callable_traits<T>::Return, typename callable_traits<T>::DecayedArgs>> : std::true_type { };

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
struct takes_caller : std::false_type { };
template <typename UI, typename First, typename... Rest>
struct takes_caller<UI, First, Rest...> : std::is_same<std::remove_cvref_t<First>, UI> { };

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
    static bool invoke(Self& self, std::string functionName, Function&& function, const std::source_location& location) {
        return self.template cppFunction<R, Args...>(std::move(functionName), std::forward<Function>(function), location);
    }
};

/// The TypeScript type of what a callback that answers later resolves its Promise with: the result of a task
/// (ResultTask<R>), the single value of another sender when its completions can be read without an environment, else `unknown`.
template <typename Sender>
struct DeferredTypescript {
    static std::string name() {
        if constexpr (requires { typename deferred_value_t<Sender>::type; })
            return inspect::typescriptType<typename deferred_value_t<Sender>::type>();
        else
            return "unknown";
    }
};
template <typename Result, typename Environment>
struct DeferredTypescript<networking::ex::task<Result, Environment>> {
    static std::string name() {
        return inspect::typescriptType<Result>();
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

/// WebFront's own rejection of a call, with one of the codes of the bridge (bridge_code). Internal: the public ways to
/// reject (BridgeError thrown by a callback, WebLink::sendError) can never carry a reserved code.
template <typename Net, http::BuffersPolicyType Policy, typename Link>
void rejectWithBridgeCode(Link& link, msg::CallId callId, std::string_view code, const std::string& message) {
    if (callId != 0)
        link.sendFrame(makeFailureFrame<Net, Policy>(callId, makeBridgeError(code, message)));
}

/// A self-contained return message carrying `value`; throws when the value cannot be encoded (a string over the message size).
template <typename Net, http::BuffersPolicyType Policy, typename Value>
websocket::Frame<Net> encodeValueFrame(msg::CallId callId, const Value& value) {
    msg::FunctionReturn<Policy> result;
    result.setCallId(callId);
    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(result.header().data()), result.header().size())};
    if constexpr (!std::is_void_v<Value>)
        result.encodeParameter(value, frame);
    frame.freeze();
    return frame;
}

/// A self-contained return message carrying `value`, or the failure of its encoding.
template <typename Net, http::BuffersPolicyType Policy, typename Value>
websocket::Frame<Net> makeValueFrame(msg::CallId callId, const Value& value) {
    try {
        return encodeValueFrame<Net, Policy>(callId, value);
    } catch (const std::exception& error) {
        return makeFailureFrame<Net, Policy>(callId, error);
    }
}

/// A return message without value.
template <typename Net, http::BuffersPolicyType Policy>
websocket::Frame<Net> makeVoidFrame(msg::CallId callId) {
    msg::FunctionReturn<Policy> result;
    result.setCallId(callId);
    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(result.header().data()), result.header().size())};
    frame.freeze();
    return frame;
}

/// `record`, when the function has one, counts each call and its failure, whether it comes from the arguments, from the
/// callback or from the encoding of its value (ADR-027, section 7).
template <typename Net, http::BuffersPolicyType Policy, typename R, typename Callable>
auto makeCppFunctionResponder(Callable&& callable, std::shared_ptr<inspect::FunctionRecord> record = {}) {
    return [callable = std::forward<Callable>(callable),
            record   = std::move(record)](std::span<const std::byte> data, auto& link, msg::CallId callId, auto&&... caller) mutable {
        msg::FunctionReturn<Policy> result;
        result.setCallId(callId);
        websocket::Frame<Net>      frame{std::span(reinterpret_cast<const std::byte*>(result.header().data()), result.header().size())};
        const inspect::CallMeasure measure{record.get()};  // the handler owns the record while it runs
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
            measure.failed(error.what());
            frame = makeFailureFrame<Net, Policy>(callId, error);
        } catch (...) {
            measure.failed("Unknown C++ exception");
            frame = makeFailureFrame<Net, Policy>(callId, std::runtime_error{"Unknown C++ exception"});
        }
        if (callId != 0)
            link.sendFrame(std::move(frame));
    };
}
}  // namespace detail

namespace detail {
/// What an application that serves a document root is told once: a document root holds development files, and the
/// development path of an interface is not hand-written pages there but TypeScript/TSX in ui/ built by wf.
inline constexpr std::string_view developmentPathNote =
    "WebFront: serving local files from documentRoot (development only). The intended development path is a TypeScript/TSX "
    "interface in ui/, built with `wf dev` in the terminal of app.bootstrap(\"developer\"); an agent can run wf through MCP "
    "(docs/DEVELOPING_WITH_AGENTS.md).";

/// Set once the process has written developmentPathNote.
inline std::atomic_flag developmentPathNoted;

/// Where the note goes: std::cerr when null. Tests point it at a stream of their own.
inline std::ostream* developmentPathOutput = nullptr;

/// Writes developmentPathNote unless it was written already. @return whether this call wrote it
inline bool noteDevelopmentPath() {
    if (developmentPathNoted.test_and_set())
        return false;
    (developmentPathOutput ? *developmentPathOutput : std::cerr) << developmentPathNote << '\n';
    return true;
}
}  // namespace detail

/// Returns `text` when it is a port number: decimal digits, from 0 to 65535, without a sign or a leading zero ("0" asks for a
/// free port). Anything else (a service name such as "http", "00", "65536") throws std::invalid_argument naming `origin`,
/// because the resolver would accept some of those and bind a port other than the one announced.
[[nodiscard]] inline std::string validatedPort(std::string_view text, std::string_view origin) {
    unsigned value = 0;
    bool     valid = !text.empty() && text.size() <= 5 && (text.size() == 1 || text.front() != '0');
    for (const char c : text) {
        valid = valid && c >= '0' && c <= '9';
        if (valid)
            value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (!valid || value > 65535)
        throw std::invalid_argument(std::string(origin) + " must be a port number from 0 to 65535 (0 asks for a free port), not '" + std::string(text) + "'");
    return std::string(text);
}

/// The port that the environment variable WEBFRONT_PORT asks for, when it is set and not empty ("0" asks for a free port). A
/// value that is not a port number throws std::invalid_argument.
[[nodiscard]] inline std::optional<std::string> environmentPort() {
    std::string value;
#ifdef _WIN32
    // std::getenv is deprecated for MSVC (C4996, an error with /WX); _dupenv_s copies the value.
    char*  buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, "WEBFRONT_PORT") == 0 && buffer != nullptr) {
        value = buffer;
        std::free(buffer);
    }
#else
    if (const char* found = std::getenv("WEBFRONT_PORT"))
        value = found;
#endif
    if (value.empty())
        return std::nullopt;
    return validatedPort(value, "WEBFRONT_PORT");
}

/// The port of a configuration that does not choose one: WEBFRONT_PORT when it is set, "9002" otherwise. A server created
/// from such a configuration (an empty `WebFrontConfig::port`) also falls back to a free port when "9002" is taken.
[[nodiscard]] inline std::string defaultPort() {
    return environmentPort().value_or("9002");
}

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
    std::string address{"127.0.0.1"};
    /// "0" requests an ephemeral port with TCPStdnet. The frontend receives the bound port. Empty (the default) takes
    /// defaultPort(): WEBFRONT_PORT, else 9002, and then, 9002 being taken, a free port, so that several applications start side
    /// by side. A port that is chosen, here or by WEBFRONT_PORT, is never replaced: when it is taken the start fails.
    std::string           port{};
    std::filesystem::path documentRoot{};
    ContentPolicy         contentPolicy{};
    /// Initial window of the embedded frontends; the title defaults to the executable's name.
    WindowOptions window{};
    /// Treats a connection whose peer address is the loopback as a local client (UI::isLocalClient) although it is
    /// not the view of an embedded frontend. This reintroduces a decision by address: it is unsafe on a shared machine
    /// and behind a local proxy or an SSH tunnel, which also arrive from the loopback. Off by default (ADR-020).
    bool trustLoopbackClients = false;
    /// How the application declares that a connection other than the view of an embedded frontend (the system browser
    /// among them) reaches it, which decides whether such a connection may read files (ADR-020 rule 3). Never deduced
    /// from an address or a `Forwarded` header. `loopbackAttested` is refused at construction unless `address` is a
    /// loopback address; the other declarations are the integrator's word, which WebFront does not verify.
    RemoteFileTransport remoteFileTransport = RemoteFileTransport::refused;
    /// Allows the optional PtyPage component. Requires a literal loopback bind and a bridge policy limited to the application's Origin.
    bool allowPtyPages = false;
    /// PEM identity for the optional HTTPS provider (webfront/tls/WebFront.hpp).
    std::shared_ptr<tls::Credentials> tlsCredentials{};
};

namespace detail {
/// Binds the server of a configuration: its port, or, when the configuration leaves the port empty and WEBFRONT_PORT is not
/// set, 9002 and then a free port if 9002 is taken. Any other failure, and any chosen port in use, propagates.
template <typename ServerType>
ServerType makeHttpServer(const WebFrontConfig& config, std::string_view preferred = "9002") {
    if (!config.port.empty())
        return ServerType(config.address, config.port, config.documentRoot, config.contentPolicy, {}, config.tlsCredentials);
    const auto chosen = environmentPort();
    if (chosen)
        return ServerType(config.address, *chosen, config.documentRoot, config.contentPolicy, {}, config.tlsCredentials);
    try {
        return ServerType(config.address, std::string(preferred), config.documentRoot, config.contentPolicy, {}, config.tlsCredentials);
    } catch (const std::system_error& error) {
        if (error.code() != std::errc::address_in_use)
            throw;
    }
    return ServerType(config.address, "0", config.documentRoot, config.contentPolicy, {}, config.tlsCredentials);
}
}  // namespace detail

namespace detail {
/// True when `address` parses as the IPv6 address ::1 in any spelling (::1, 0:0:0:0:0:0:0:1, 0::0001...).
[[nodiscard]] inline bool isIPv6Loopback(std::string_view address) {
    if (address.find(':') == std::string_view::npos)
        return false;
    const auto compress = address.find("::");
    if (compress != std::string_view::npos && address.find("::", compress + 1) != std::string_view::npos)
        return false;
    const auto groups = [](std::string_view text, std::vector<unsigned>& out) {
        while (!text.empty()) {
            const auto colon = text.find(':');
            const auto group = text.substr(0, colon);
            if (group.empty() || group.size() > 4)
                return false;
            unsigned value = 0;
            for (const char c : group) {
                const unsigned digit = c >= '0' && c <= '9'   ? static_cast<unsigned>(c - '0')
                                       : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                       : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                              : 16u;
                if (digit > 15)
                    return false;
                value = value * 16 + digit;
            }
            out.push_back(value);
            if (colon == std::string_view::npos)
                break;
            text.remove_prefix(colon + 1);
            if (text.empty())
                return false;  // a trailing single colon
        }
        return true;
    };
    std::vector<unsigned> head, tail;
    if (compress == std::string_view::npos) {
        if (!groups(address, head) || head.size() != 8)
            return false;
    } else {
        if (!groups(address.substr(0, compress), head) || !groups(address.substr(compress + 2), tail) || head.size() + tail.size() > 7)
            return false;
    }
    std::vector<unsigned> all = head;
    all.insert(all.end(), 8 - head.size() - tail.size(), 0u);
    all.insert(all.end(), tail.begin(), tail.end());
    return all.size() == 8
           && std::all_of(all.begin(),
                          all.end() - 1,
                          [](unsigned g) {
                              return g == 0;
                          })
           && all.back() == 1;
}

/// True for the literal loopback addresses a server can be configured with: 127.x.y.z and ::1 in any spelling, bracketed or not.
[[nodiscard]] inline bool isLoopbackName(std::string_view address) {
    if (address.size() > 2 && address.front() == '[' && address.back() == ']')
        address = address.substr(1, address.size() - 2);
    // "localhost" is a name, resolved later by the server to whatever the host says: only literal addresses are verifiable here.
    if (isIPv6Loopback(address))
        return true;
    if (!address.starts_with("127."))
        return false;
    // Four decimal octets of 0 to 255 without leading zeros: the string must read the same to every resolver (127.256.0.1 or
    // 127.0.0.010 are not literal addresses, and a resolver could take them for names).
    int      octets      = 0;
    unsigned value       = 0;
    int      digits      = 0;
    bool     leadingZero = false;
    for (const char c : address) {
        if (c == '.') {
            if (digits == 0)
                return false;
            ++octets;
            value       = 0;
            digits      = 0;
            leadingZero = false;
        } else if (c >= '0' && c <= '9') {
            if (digits == 1 && leadingZero)
                return false;
            leadingZero = digits == 0 && c == '0';
            value       = value * 10 + static_cast<unsigned>(c - '0');
            if (++digits > 3 || value > 255)
                return false;
        } else {
            return false;
        }
    }
    return octets == 3 && digits > 0;
}

/// Refuses the configurations whose declaration WebFront can disprove, before anything is bound.
inline const std::string& checkedAddress(const WebFrontConfig& config) {
    if (config.remoteFileTransport == RemoteFileTransport::loopbackAttested && !isLoopbackName(config.address))
        throw std::invalid_argument("remoteFileTransport::loopbackAttested needs a server that listens on the loopback only, not '" + config.address + "'");
    return config.address;
}

inline WebFrontConfig checkedPtyPageConfig(WebFrontConfig config) {
    if (config.allowPtyPages) {
        if (!isLoopbackName(config.address) || (config.address.contains(':') && config.address != "::1"))
            throw std::invalid_argument("PTY pages require a literal loopback listening address (127.x.y.z or ::1)");
        if (config.contentPolicy.allowMissingOrigin || config.contentPolicy.allowAnyBridgeOrigin || !config.contentPolicy.bridgeAllowedOrigins.empty())
            throw std::invalid_argument("PTY pages require a bridge policy limited to the application's own Origin");
    }
    return config;
}

/// The read binding of a configuration, which is refused here, before anything is bound, if its declaration is false.
inline std::shared_ptr<ReadBinding> makeReadBinding(const WebFrontConfig& config, bool secure = false) {
    checkedAddress(config);
    if (secure && !isLoopbackName(config.address))
        std::cerr << "WebFront: TLS listener outside loopback. Handshake cryptography runs on the I/O thread, "
                     "and bursts can delay established connections or cause denial of service. See docs/TLS.md.\n";
    return std::make_shared<ReadBinding>(ReadBinding::Options{.remote = config.remoteFileTransport, .secure = secure});
}
}  // namespace detail

template <typename NetProvider,
          typename Filesystem,
          http::BuffersPolicyType Policy   = http::DefaultBuffersPolicy,
          frontend::FrontendType  Frontend = frontend::DefaultFrontend>
class BasicWF {
    template <typename>
    friend class BasicSpace;

public:
    using Net              = NetProvider;
    using BufferPolicy     = Policy;
    using FrontendProvider = Frontend;
    using UI               = BasicUI<BasicWF<Net, Filesystem, Policy, Frontend>>;
    /// The part of the application that one user's pages see when it serves several (see spaces()).
    using Space = BasicSpace<BasicWF>;
    /// Decides, for a segment under a family's prefix that has no space, to create it (`Space{app, prefix, segment}`) and
    /// return nothing, or to refuse it with the reply returned (404 for an unknown session, 503 for a full server...).
    using SpaceResolver = std::function<std::optional<http::RouteReply>(std::string_view segment)>;

private:
    using CppHandler        = std::function<void(std::span<const std::byte>, WebLink<Net, Policy>&, msg::CallId, UI)>;
    using SpaceState        = detail::SpaceState<UI, CppHandler>;
    using SpaceRegistryType = detail::SpaceRegistry<SpaceState>;

public:
    // Newcomer-friendly defaults: loopback address, port 9002 (a free one when it is taken), no local document root.
    BasicWF() : BasicWF(WebFrontConfig{}) { }

    explicit BasicWF(WebFrontConfig config) : BasicWF(detail::checkedPtyPageConfig(std::move(config)), CheckedConfig{}) { }

private:
    struct CheckedConfig { };
    BasicWF(WebFrontConfig config, CheckedConfig)
        : readBinding(detail::makeReadBinding(config, networking::secureTransport<Net>)),
          httpServer((detail::ensureFrontendInitialized<Frontend>(), detail::makeHttpServer<http::Server<Net, Filesystem, Policy>>(config))),
          httpAddress(config.address),
          httpPort(httpServer.port()),
          httpDocRoot(config.documentRoot),
          contentPolicy(std::move(config.contentPolicy)),
          windowOptions(std::move(config.window)),
          trustLoopbackClients(config.trustLoopbackClients),
          ptyPagesAllowed(config.allowPtyPages),
          idsCounter(0) {
        // Links are created on the server thread, outside the lock: identifiers are only assigned here.
        httpServer.onUpgrade([this](typename Net::Socket&& socket, const http::UpgradeSpace& resolved) {
            WebLinkId         id{};
            WebLinkGeneration generation{};
            // The very space resolved for the upgrade request, never another that took its segment since: one destroyed
            // meanwhile (checked below, under the lock its destruction takes) leaves the socket to close.
            auto space = std::static_pointer_cast<SpaceState>(resolved.target);
            if (!resolved.path.empty() && !space)
                return;
            {
                std::scoped_lock lock{linksMutex};
                if (space && !space->alive)
                    return;
                while (webLinks.contains(idsCounter))
                    ++idsCounter;
                id         = idsCounter++;
                generation = ++generationCounter;
            }
            bool loopbackPeer = false;
            if constexpr (requires(std::error_code& error) { socket.remote_endpoint(error).address().is_loopback(); }) {
                if (trustLoopbackClients) {
                    std::error_code error;
                    const auto      peer = socket.remote_endpoint(error);
                    loopbackPeer         = !error && detail::isLoopbackAddress(peer.address());
                }
            }
            auto link = std::make_shared<WebLink<Net, Policy>>(
                std::move(socket),
                id,
                [this](WebLinkEvent event) {
                    onEvent(event);
                },
                httpServer.taskScope(),
                false);
            {
                std::scoped_lock lock{linksMutex};
                if (space) {
                    if (!space->alive)
                        return;  // destroyed meanwhile: the link, never started, closes the socket
                    ++space->pages;
                }
                webLinks.emplace(id,
                                 Connection{.link           = link,
                                            .generation     = generation,
                                            .thread         = std::this_thread::get_id(),
                                            .closed         = false,
                                            .stage          = Connection::Stage::handshaking,
                                            .deferred       = {},
                                            .viewAssociated = false,
                                            .loopbackPeer   = loopbackPeer,
                                            .space          = std::move(space),
                                            .path           = resolved.requestPath});
            }
            link->start();  // registered first: every event it dispatches finds it
        });
        // The exchange of the binding secret for the client cookie; the secret is part of the path, so it is never logged.
        readBinding->useNamespace(httpPort);
        bindRoute = httpServer.addRoute(std::string(ReadBinding::bindPath), readBinding->exchangeHandler(), true);
    }

public:
    [[nodiscard]] bool allowsPtyPages() const noexcept {
        return ptyPagesAllowed;
    }

    // Preserved for source compatibility; binds all interfaces like it always has, unlike the
    // loopback-by-default WebFrontConfig constructor above.
    explicit BasicWF(std::string_view port, std::filesystem::path docRoot = ".")
        : BasicWF(WebFrontConfig{.address = "0.0.0.0", .port = std::string(port), .documentRoot = std::move(docRoot)}) { }

    // Each UI and the server callbacks hold the application's address: it never moves.
    BasicWF(const BasicWF&)            = delete;
    BasicWF& operator=(const BasicWF&) = delete;

    /** @brief The listening port, including the OS-assigned TCPStdnet port when configured with "0". */
    std::string_view port() const noexcept {
        return httpPort;
    }

    ~BasicWF() noexcept {
        {
            // A space destroyed after its application only releases what it holds.
            std::scoped_lock lock{spaceRegistry->mutex};
            spaceRegistry->attached = false;
        }
        stopAndJoinServer();
        // Coroutine cleanup may use its UI, so links stay alive through the join.
        httpServer.drainUntilJoined();
        releaseLinks();
        closePtyPages();
    }

    /** @brief Stops and joins owned work before returning. Call outside server callbacks,
     * after any external callers of run()/runOne() have returned. Safe to repeat.
     */
    void shutdown() {
        stopAndJoinServer();
        // Application coroutine cleanup may still use its UI: retain links until join.
        httpServer.drain();
        releaseLinks();
        closePtyPages();
    }

    /** @brief Thread-safe while the application lives. Starts on the server thread.
     * Rejected calls return a stopped handle without starting the body.
     * Accepted tasks can start before shutdown cancels and drains them.
     * Ignoring the returned handle keeps the task running until completion or shutdown.
     */
    TaskHandle spawn(Task task) {
        return httpServer.startTask(std::move(task));
    }

    /** @brief Answers the `GET` and `HEAD` requests under `prefix` (absolute, ending with '/') with a reply produced at
     * run time and streamed in chunks of bounded size (see http::RouteReply). The handler runs on a worker thread, not on
     * the server context. The route precedes the file providers, never masks the embedded `WebFront.js`, and lasts as long
     * as the returned registration; an overlapping prefix throws std::invalid_argument. Thread-safe.
     */
    [[nodiscard]] http::RouteTable::Registration addRoute(std::string prefix, http::RouteHandler handler) {
        return httpServer.addRoute(std::move(prefix), std::move(handler));
    }

    /** @brief The same, with the options of http::RouteOptions: `postBodyLimit` makes the route answer `POST` too, with a
     * body bounded by `Content-Length` and given to the handler whole (RouteRequest::body). See docs/HTTP_ASSETS.md.
     */
    [[nodiscard]] http::RouteTable::Registration addRoute(std::string prefix, http::RouteHandler handler, http::RouteOptions options) {
        return httpServer.addRoute(std::move(prefix), std::move(handler), std::move(options));
    }

    /** @brief Declares that the segment following `prefix` (absolute, ending with '/', such as "/s/") is a secret, a capability
     * such as a session's identifier: WebFront never writes it to its logs, for files, routes, WebSocket upgrades and their
     * failures alike ("/s/<id>/index.html" is logged with a star in place of `<id>`), nor the message of an error raised
     * while serving such a path. A family of spaces (spaces()) always masks its segment so. Throws std::invalid_argument
     * for a malformed prefix. Thread-safe; declare the prefixes before the pages can request them.
     */
    void addSecretPrefix(std::string_view prefix) {
        httpServer.addSecretPrefix(prefix);
    }

    /** @brief Declares a family of spaces under `prefix` (absolute, ending with '/', such as "/s/"): an HTTP request or a
     * WebSocket whose path starts with `prefix + segment + "/"` belongs to the space `segment` (see BasicSpace), and is
     * served as its path without that start would be at the root: a page with relative URLs works at `/s/abc/` as at `/`.
     *
     * `resolver` is called on the server context for a segment that has no space: it creates the space, which the
     * application owns (`std::make_unique<Space>(app, prefix, segment)`), and returns nothing, or returns the reply that
     * refuses it, which reaches the browser; returning nothing without creating the space answers 404. A space may also be
     * created beforehand, outside the resolver. Without a call to spaces(), nothing changes for the application. Throws
     * std::invalid_argument for a malformed prefix, a prefix that overlaps another family's, or an empty resolver.
     * Thread-safe; declare the families before the pages can request them.
     */
    void spaces(std::string prefix, SpaceResolver resolver) {
        if (!resolver)
            throw std::invalid_argument("A family of spaces needs a resolver");
        httpServer.mount(prefix,
                         [this, prefix, resolve = std::make_shared<const SpaceResolver>(std::move(resolver))](std::string_view segment) -> http::MountAnswer {
                             const auto path   = prefix + std::string(segment) + "/";
                             const auto routes = [&]() -> std::optional<http::MountAnswer> {
                                 if (const auto found = spaceRegistry->find(path))
                                     return http::MountAnswer{.routes = found->routeTable(), .refusal = {}, .target = found};
                                 return std::nullopt;
                             };
                             if (auto existing = routes())
                                 return std::move(*existing);
                             if (!detail::validSpaceSegment(segment))
                                 return {.routes = nullptr, .refusal = http::textReply(http::Response::notFound, "Not Found\n"), .target = nullptr};
                             if (auto refusal = (*resolve)(segment))
                                 return {.routes = nullptr, .refusal = std::move(refusal), .target = nullptr};
                             if (auto created = routes())
                                 return std::move(*created);
                             return {.routes = nullptr, .refusal = http::textReply(http::Response::notFound, "Not Found\n"), .target = nullptr};
                         });
        std::scoped_lock lock{spaceRegistry->mutex};
        spaceRegistry->families.push_back(std::move(prefix));
    }

    /** @brief Registers a route whose URLs are read grants (UI::grantRead): `handler` is called only for a request that
     * presents a valid token and, unless the grant is transferable, the cookie of the client of the connection that owns
     * the grant (ADR-020, rule 4). Every answer is neutralized: `no-store`, `no-referrer`, `nosniff`, a sandboxing
     * Content-Security-Policy, and an attachment disposition outside the passive types; HTML, XML, JavaScript and
     * WebAssembly are announced as application/octet-stream. `prefix` is under "/webfront/services/" (the cookie's
     * path) and is not "/webfront/services/bind/", which belongs to the exchange. Same rules as addRoute otherwise.
     */
    [[nodiscard]] ReadRoute addReadRoute(std::string prefix, ReadHandler handler) {
        if (!handler)
            throw std::invalid_argument("A read route needs a handler");
        if (!prefix.starts_with(ReadBinding::servicesPath) || prefix.size() == ReadBinding::servicesPath.size() || prefix.starts_with(ReadBinding::bindPath))
            throw std::invalid_argument("A read route lives under " + std::string(ReadBinding::servicesPath) + ", not under "
                                        + std::string(ReadBinding::bindPath));
        auto id           = automation::channel::nonce();  // this registration's identity: its grants work nowhere else
        auto registration = httpServer.addRoute(prefix, readBinding->readHandler(id, prefix, std::move(handler)), true, [](http::RouteReply& reply) {
            detail::hardenRead(reply, {});
        });
        return ReadRoute{std::move(id), prefix, readBinding, std::move(registration)};
    }

    /// Revokes the read grant of a URL returned by UI::grantRead (or of its token); an unknown one is ignored.
    void revokeRead(std::string_view urlOrToken) {
        readBinding->revoke(urlOrToken);
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

    /// The interface thread of the frontend: available while an embedded frontend's loop runs (or the system browser's
    /// optional loop mode), never for a frontend without one. See frontend/UIExecutor.hpp for the completion rules.
    [[nodiscard]] frontend::UIExecutor uiExecutor() const {
        if constexpr (requires {
                          { Frontend::uiExecutor() } -> std::same_as<frontend::UIExecutor>;
                      })
            return Frontend::uiExecutor();
        else
            return {};
    }

    /// The native window of the frontend (see frontend/NativeWindow.hpp), or a handle that answers `unsupported` for a
    /// frontend without one. It names the window, not a connection: UI::nativeWindow() also checks that the caller's view
    /// is that window.
    [[nodiscard]] frontend::NativeWindow nativeWindow() const {
        if constexpr (requires {
                          { Frontend::nativeWindow() } -> std::same_as<frontend::NativeWindow>;
                      })
            return Frontend::nativeWindow();
        else
            return {};
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
    template <typename>
    friend struct testing::LaunchSecret;   // lets a test present the secret an embedded view would
    template <typename Network, frontend::FrontendType DeveloperFrontend, fs::Provider... Extra>
    friend class DeveloperAppWithNetwork;  // installs the inspectors' reserved functions (reservedCppFunction)

    /// Registers one of WebFront's own reserved functions (`webfront.inspector.*`), which cppFunction refuses to the
    /// application. Only the developer environment calls it, and a name already taken is an error, never ignored: the
    /// inspectors' callbacks are WebFront's or none (ADR-027, section 1).
    template <typename R, typename... Args>
    void reservedCppFunction(std::string functionName, auto&& function) {
        // No record: the inspectors do not list their own functions, whose calls would be those of the inspector itself.
        if (!cppFunctions.try_emplace(functionName, makeCppHandler<R, Args...>(std::forward<decltype(function)>(function))).second)
            throw std::logic_error("webfront: the reserved function " + functionName + " is already registered");
    }

    /// Sends `call` to each browser not reported closed whose onUIStarted handlers have returned: at once, on the
    /// calling thread and outside every lock. For a browser whose handlers are running, `call` is kept, within its
    /// limit, and sent on the server thread as soon as they return, after what they sent and in the order of the calls.
    /// Returns once `call` is queued or kept for every browser: a call that follows it (same thread, or synchronized
    /// after it) comes after it. Overlapping calls are not ordered: holding linksMutex while writing would not order
    /// them either, and would hold a WebFront lock while the write starts.
    /// With `only`, the browsers of that space alone.
    void sendToStartedLinks(const detail::BroadcastCall<Net>& call, const SpaceState* only = nullptr) {
        std::vector<std::shared_ptr<WebLink<Net, Policy>>> started;
        {
            std::scoped_lock lock{linksMutex};
            started.reserve(webLinks.size());
            for (auto& [id, connection] : webLinks) {
                if (connection.closed || (only && connection.space.get() != only))
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
        return linkAndGeneration(id).second;
    }

    /// The link and generation of the connection that now owns `id`, read under one lock (null and 0 when none does).
    std::pair<std::shared_ptr<WebLink<Net, Policy>>, WebLinkGeneration> linkAndGeneration(WebLinkId id) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        if (found == webLinks.end())
            return {nullptr, WebLinkGeneration{0}};
        return {found->second.link, found->second.generation};
    }

    [[nodiscard]] bool isEmbeddedView(WebLinkId id, WebLinkGeneration generation) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        return found != webLinks.end() && found->second.generation == generation && !found->second.closed && found->second.viewAssociated;
    }

    [[nodiscard]] frontend::NativeWindowResult nativeWindowOf(WebLinkId id, WebLinkGeneration generation) const {
        if (!isEmbeddedView(id, generation))
            return std::unexpected(frontend::NativeWindowError::notEmbedded);
        return nativeWindow().get();
    }

    [[nodiscard]] std::string pathOf(WebLinkId id, WebLinkGeneration generation) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        return found != webLinks.end() && found->second.generation == generation ? found->second.path : std::string{};
    }

    [[nodiscard]] bool isLocalClient(WebLinkId id, WebLinkGeneration generation) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(id);
        return found != webLinks.end() && found->second.generation == generation && !found->second.closed
               && (found->second.viewAssociated || (trustLoopbackClients && found->second.loopbackPeer));
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
        forEachUIIn(nullptr, std::forward<Function>(function));
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
     * @param location where the function is registered, which the inspectors show (ADR-027, section 7): leave its default
     * @return false when a function of that name was already registered, which stays registered
     * @throws std::invalid_argument for a name starting with `webfront.inspector.`, reserved to WebFront (ADR-027)
     */
    template <typename R, typename... Args>
    bool cppFunction(std::string functionName, auto&& function, std::source_location location = std::source_location::current()) {
        refuseReservedName(functionName);
        return registerCppFunction<R, Args...>(std::move(functionName), std::forward<decltype(function)>(function), location);
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
     * @param location where the function is registered, which the inspectors show (ADR-027, section 7): leave its default
     * @return false when a function of that name was already registered, which stays registered
     * @throws std::invalid_argument for a name starting with `webfront.inspector.`, reserved to WebFront (ADR-027)
     */
    template <typename Function>
    bool cppFunction(std::string functionName, Function&& function, std::source_location location = std::source_location::current()) {
        using Callable = std::remove_cvref_t<Function>;
        static_assert(detail::has_callable_traits<Callable>,
                      "webfront::BasicWF::cppFunction: could not deduce this callable's signature. Generic "
                      "lambdas, overloaded operator(), and other advanced callables require explicit "
                      "cppFunction<ReturnType, ArgTypes...>(name, callable).");
        using Traits = detail::callable_traits<Callable>;
        using Args   = typename detail::caller_aware_args<UI, typename Traits::RawArgs, typename Traits::DecayedArgs>::type;
        return detail::ApplyArgs<typename Traits::Return, Args>::invoke(*this, std::move(functionName), std::forward<Function>(function), location);
    }

    /**
     * @brief The C++ functions registered now, the application's and its spaces', in the order of their registration: name,
     * signature written in TypeScript, where each was registered, whether it receives the calling UI or answers later, and
     * the counts of its calls (ADR-027, section 7). A copy taken at one moment; callable from any thread. The functions of
     * WebFront's own inspectors (`webfront.inspector.*`) are not listed. Empty in a distribution build, which records nothing.
     */
    [[nodiscard]] std::vector<inspect::FunctionInfo> registeredFunctions() const {
        return functionRegistry->list();
    }

    /// The counter of the calls C++ makes to the JavaScript function `functionName`, null while no inspector can read it
    /// (outside the developer environment, and in a distribution build). For the bridge's own call objects.
    [[nodiscard]] std::shared_ptr<inspect::JsCallCounter> jsCallCounter(std::string_view functionName) const {
        return jsCalls ? jsCalls->counter(functionName) : nullptr;
    }

    /// Where a call object that may start its calls later finds that counter then (JsFunctionSender); null in a
    /// distribution build. It stays valid after the application is gone.
    [[nodiscard]] const std::shared_ptr<inspect::JsCallSource>& jsCallSource() const noexcept {
        return jsCalls;
    }

    /**
     * @brief Declares a watch: an object or a container of the application that the inspectors of the developer environment
     * show as a tree and refresh (ADR-027, section 5; docs/WATCHES.md). The watch exists while the returned handle lives.
     *
     * `source` reaches the object, and every operation of the inspectors (a page, a pass of differences, a value, a write)
     * runs whole inside the access it defines:
     * - a visitor, `[&](webfront::inspect::Visit<const T> visit) { std::scoped_lock lock{m}; visit(object); }`, is called by
     *   WebFront and calls the visit once with the object under its own lock: it is the form of an object shared between
     *   threads, and `Visit<T>` makes the watch writable;
     * - an accessor, `[&] -> const T& { return object; }`, returns a reference, read on the thread that runs the operation
     *   (correct only for an object that thread alone touches), or a copy. A lock taken inside an accessor protects nothing.
     *
     * `executor` (`app.uiExecutor()`, a host's loop: anything whose `schedule()` gives a sender) is the thread that owns the
     * object: the source and the whole operation run there, in the order the calls were received. Without one they run on
     * the server thread, the one of the `cppFunction` callbacks. An accessor that returns a non-constant reference is
     * writable with an executor only.
     *
     * `mutator`, `[](T& object, std::string_view path, std::string_view text) { ... }`, applies a write its own way and
     * throws to refuse it; without one a value is assigned. Values are writable only when the source is.
     *
     * Destroying the handle waits for an operation that another thread started: destroy it outside every lock the source
     * takes and outside the executor. Destroying it from the operation itself terminates the process, after a diagnostic.
     * The overloads that take a `std::shared_ptr<State>` never wait and allow both: see below.
     *
     * Outside the developer environment (`bootstrap("developer")`), and in a distribution build, nothing is registered and
     * the handle is empty: declare the watches after bootstrap(). Inside, a watch costs an entry in a table until an
     * inspector reads it. Callable from any thread.
     *
     * @throws std::invalid_argument for an empty name, a name longer than 256 bytes, or the name of a live watch
     */
    template <inspect::WatchSource Source>
    [[nodiscard]] inspect::Watch watch(std::string name, Source&& source, std::source_location location = std::source_location::current()) {
        using Shape = inspect::SourceShape<std::decay_t<Source>>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   std::shared_ptr<inspect::NoState>{},
                                   inspect::NoExecutor{},
                                   inspect::NoMutator{});
    }
    template <inspect::WatchSource Source, inspect::WatchExecutor Executor>
    [[nodiscard]] inspect::Watch watch(std::string name, Source&& source, Executor executor, std::source_location location = std::source_location::current()) {
        using Shape = inspect::SourceShape<std::decay_t<Source>>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   std::shared_ptr<inspect::NoState>{},
                                   std::move(executor),
                                   inspect::NoMutator{});
    }
    template <inspect::WatchSource Source, inspect::WatchMutator<typename inspect::SourceShape<std::decay_t<Source>>::Object> Mutator>
    [[nodiscard]] inspect::Watch watch(std::string name, Source&& source, Mutator mutator, std::source_location location = std::source_location::current()) {
        using Shape = inspect::SourceShape<std::decay_t<Source>>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   std::shared_ptr<inspect::NoState>{},
                                   inspect::NoExecutor{},
                                   std::move(mutator));
    }
    template <inspect::WatchSource                                                               Source,
              inspect::WatchExecutor                                                             Executor,
              inspect::WatchMutator<typename inspect::SourceShape<std::decay_t<Source>>::Object> Mutator>
    [[nodiscard]] inspect::Watch
    watch(std::string name, Source&& source, Executor executor, Mutator mutator, std::source_location location = std::source_location::current()) {
        using Shape = inspect::SourceShape<std::decay_t<Source>>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   std::shared_ptr<inspect::NoState>{},
                                   std::move(executor),
                                   std::move(mutator));
    }

    /**
     * @brief Declares a watch whose source owns its state: each operation keeps `owner` alive while it runs, and the source
     * captures nothing else by reference. It receives the state, `[](State& state, inspect::Visit<T> visit) {
     * std::scoped_lock lock{state.mutex}; visit(state.value); }` (or `[](State& state) -> T& { ... }`).
     *
     * Destroying the handle then never waits: it may be destroyed under the lock the source takes, and from the operation
     * itself (source, visitor or mutator). An operation in progress ends on a state that still lives, and its answer is
     * replaced by the refusal `webfront.inspector.unknown`. It is the form to choose when the order of destruction cannot
     * be guaranteed. The other parameters are those of the overloads above.
     */
    template <typename State, inspect::OwnedWatchSource<State> Source>
    [[nodiscard]] inspect::Watch
    watch(std::string name, std::shared_ptr<State> owner, Source&& source, std::source_location location = std::source_location::current()) {
        using Shape = inspect::OwnedSourceShape<std::decay_t<Source>, State>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   requireOwner(std::move(owner)),
                                   inspect::NoExecutor{},
                                   inspect::NoMutator{});
    }
    template <typename State, inspect::OwnedWatchSource<State> Source, inspect::WatchExecutor Executor>
    [[nodiscard]] inspect::Watch
    watch(std::string name, std::shared_ptr<State> owner, Source&& source, Executor executor, std::source_location location = std::source_location::current()) {
        using Shape = inspect::OwnedSourceShape<std::decay_t<Source>, State>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   requireOwner(std::move(owner)),
                                   std::move(executor),
                                   inspect::NoMutator{});
    }
    template <typename State,
              inspect::OwnedWatchSource<State>                                                               Source,
              inspect::WatchMutator<typename inspect::OwnedSourceShape<std::decay_t<Source>, State>::Object> Mutator>
    [[nodiscard]] inspect::Watch
    watch(std::string name, std::shared_ptr<State> owner, Source&& source, Mutator mutator, std::source_location location = std::source_location::current()) {
        using Shape = inspect::OwnedSourceShape<std::decay_t<Source>, State>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   requireOwner(std::move(owner)),
                                   inspect::NoExecutor{},
                                   std::move(mutator));
    }
    template <typename State,
              inspect::OwnedWatchSource<State>                                                               Source,
              inspect::WatchExecutor                                                                         Executor,
              inspect::WatchMutator<typename inspect::OwnedSourceShape<std::decay_t<Source>, State>::Object> Mutator>
    [[nodiscard]] inspect::Watch watch(std::string            name,
                                       std::shared_ptr<State> owner,
                                       Source&&               source,
                                       Executor               executor,
                                       Mutator                mutator,
                                       std::source_location   location = std::source_location::current()) {
        using Shape = inspect::OwnedSourceShape<std::decay_t<Source>, State>;
        return declareWatch<Shape>(std::move(name),
                                   location,
                                   std::forward<Source>(source),
                                   requireOwner(std::move(owner)),
                                   std::move(executor),
                                   std::move(mutator));
    }

private:
    template <typename State>
    static std::shared_ptr<State> requireOwner(std::shared_ptr<State> owner) {
        if (!owner)
            throw std::invalid_argument("webfront::BasicWF::watch(): an owning watch needs the state it owns");
        return owner;
    }

    template <typename Shape, typename Source, typename State, typename Executor, typename Mutator>
    [[nodiscard]] inspect::Watch
    declareWatch(std::string name, const std::source_location& location, Source&& source, std::shared_ptr<State> owner, Executor executor, Mutator mutator) {
        inspect::WatchRegistry::checkName(name);
        if constexpr (inspect::introspection) {
            const auto registry = inspectorRegistry();
            if (!registry)
                return {};
            const auto id = registry->add(inspect::makeWatch<Shape>(std::move(name),
                                                                    inspect::locationText(location),
                                                                    std::forward<Source>(source),
                                                                    std::move(owner),
                                                                    std::move(executor),
                                                                    std::move(mutator)));
            return inspect::Watch{registry, id};
        } else {
            static_cast<void>(location), static_cast<void>(source), static_cast<void>(owner), static_cast<void>(executor), static_cast<void>(mutator);
            return {};
        }
    }

    /// The names of the inspectors belong to WebFront: an application can neither hide one nor pose as one (ADR-027, section 1).
    static void refuseReservedName(std::string_view functionName) {
        if (inspect::isReservedName(functionName))
            throw std::invalid_argument("webfront::BasicWF::cppFunction(): names starting with 'webfront.inspector.' are reserved to WebFront");
    }

    template <typename R, typename... Args>
    bool registerCppFunction(std::string functionName, auto&& function, const std::source_location& location) {
        if (cppFunctions.contains(functionName))
            return false;
        // The record is listed once the registration is accepted: nothing reads a function that is not registered.
        const auto record = recordFunction<R, Args...>(functionName, location);
        const bool added =
            cppFunctions.try_emplace(std::move(functionName), makeCppHandler<R, Args...>(std::forward<decltype(function)>(function), record)).second;
        if (added)
            publishFunction(record);
        return added;
    }

    /// Lists the record of a function whose registration was accepted (null in a distribution build).
    void publishFunction(const std::shared_ptr<inspect::FunctionRecord>& record) {
        if (record)
            functionRegistry->publish(record);
    }

    /// What the inspectors keep of a function about to be registered (ADR-027, section 7), owned by its handler and not
    /// listed until publishFunction(): a registration that is refused, or a function released with its space, takes its
    /// record with it. Null in a distribution build. `scope` is the path of the space that registers it.
    template <typename R, typename... Args>
    [[nodiscard]] std::shared_ptr<inspect::FunctionRecord>
    recordFunction(std::string_view functionName, const std::source_location& location, std::string_view scope = {}) {
        if constexpr (inspect::introspection) {
            constexpr bool           caller   = detail::takes_caller<UI, Args...>::value;
            constexpr bool           deferred = detail::deferred_result<R>;
            std::vector<std::string> parameters{inspect::typescriptType<Args>()...};
            if (caller)
                parameters.erase(parameters.begin());  // the calling UI does not come from JavaScript
            std::string result;
            if constexpr (deferred)
                result = detail::DeferredTypescript<std::remove_cvref_t<R>>::name();
            else
                result = inspect::typescriptType<R>();
            return functionRegistry->prepare(functionName, inspect::typescriptSignature(parameters, result), caller, deferred, location, scope);
        } else {
            return nullptr;
        }
    }

public:
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
        // The thread of this application's show(): requestClose() recognizes the session that is its own by it.
        // Only the show() that takes the frontend's session ownership can be asked to close; one that finds it taken (another
        // application's window, or its own interface thread calling show() again) is refused below, before the frontend.
        struct Showing {
            BasicWF& self;
            bool     owns;
            explicit Showing(BasicWF& owner) : self(owner), owns(false) {
                const void* none = nullptr;
                owns             = detail::FrontendSessionOwner<Frontend>::owner.compare_exchange_strong(none, &self);
                if (owns) {
                    self.showThread = std::this_thread::get_id();
                    ++self.showEpoch;  // odd: owns the session
                }
            }
            ~Showing() {
                if (owns) {
                    ++self.showEpoch;  // even: a reader that saw the odd value and sees this one reads nothing
                    self.showThread                               = std::thread::id{};
                    detail::FrontendSessionOwner<Frontend>::owner = nullptr;
                }
            }
        } showing{*this};
        // A frontend that can be asked to close has one primary session at a time: a show() that did not take the ownership must
        // not open one that nobody could close (the first may end before this one attaches, and the frontend would accept it).
        if constexpr (frontend::endsOnRequest<Frontend>) {
            if (!showing.owns)
                throw std::logic_error("webfront::BasicWF::show(): another application is already showing a window through this frontend");
        }
        constexpr bool acceptsViewLaunch = requires(const ViewLaunch& launch) {
            Frontend::open(httpAddress, httpPort, htmlFilename, contentPolicy, windowOptions, launch);
        };
        if constexpr (networking::secureTransport<Net>)
            static_assert(acceptsViewLaunch, "An HTTPS frontend must accept ViewLaunch in open()");
        if constexpr (acceptsViewLaunch)
            Frontend::open(httpAddress,
                           httpPort,
                           htmlFilename,
                           contentPolicy,
                           windowOptions,
                           ViewLaunch{viewSecret, detail::browserOrigin(httpAddress, httpPort, networking::secureTransport<Net>)});
        else if constexpr (requires { Frontend::open(httpAddress, httpPort, htmlFilename, contentPolicy, windowOptions); })
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
        if (launched)
            throw std::logic_error("webfront::BasicWF::show(): this application was started with start(); it has no window loop to run");
        // One show() at a time per application: a second call (from another thread, or re-entrantly from the interface
        // thread) is refused before it touches anything. Its error path stops the server, which belongs to the first call.
        if (showRunning.exchange(true))
            throw std::logic_error("webfront::BasicWF::show(): this application is already showing; its server and window are left untouched");
        struct ShowDone {
            std::atomic<bool>& running;
            ~ShowDone() {
                running = false;
            }
        } showDone{showRunning};
        shown = true;
        startServerThread();
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
        // Once per process, and never for an application whose document root is the project of its developer environment.
        if (!httpDocRoot.empty() && !developerEnvironment)
            detail::noteDevelopmentPath();
        openAndRun(page);
    }

    /** @brief Starts the server on its own thread, opens the page and returns at once: the calling thread keeps its
     * own loop (docs/HOST_INTEGRATION.md). Nothing is read from the console and nothing waits for the interface.
     *
     * Only for a frontend that can open without running a loop, which it declares with `launch()`: the system browser
     * (the default `webfront::WebFront`). An embedded window lives in the loop of `show()`; such a frontend is refused at
     * compile time. Call it once per application, on the thread that owns it, instead of show(): a second start(), or
     * a start() or show() after the other (also once show() has stopped its server), throws std::logic_error and leaves the running server untouched.
     * If the opening throws, the server is stopped and joined before the exception propagates, as for show(); that
     * application cannot be started again (the stopped server is not re-armed): construct another. Otherwise the
     * server runs until stop(), shutdown() or destruction; register callbacks and handlers before the call.
     *
     * The frontend receives the address, the port, the page and, when its launch() takes it, the content policy. It gets
     * no WindowOptions and no view-association proof: an interface that is launched and left is an external browser,
     * which show() does not give them either (ADR-020).
     */
    void start(std::string_view page = "index.html") {
        constexpr bool secure    = networking::secureTransport<Net>;
        constexpr bool canLaunch = secure ? requires { Frontend::launchHttps(std::string_view{}, std::string_view{}, std::string_view{}); }
                                          : requires { Frontend::launch(std::string_view{}, std::string_view{}, std::string_view{}); };
        if constexpr (secure)
            static_assert(canLaunch, "webfront::BasicWF::start(): an HTTPS frontend must provide launchHttps(address, port, page)");
        else
            static_assert(canLaunch,
                          "webfront::BasicWF::start(): this frontend opens its window inside the loop of show(), which keeps the "
                          "calling thread. Call show() on the interface thread and end it with requestClose(), or run the "
                          "application in a webfront::UIThread when the frontend allows a secondary thread "
                          "(docs/HOST_INTEGRATION.md).");
        if constexpr (canLaunch) {
            if (shown || serverThread.joinable() || launched.exchange(true))
                throw std::logic_error("webfront::BasicWF::start(): the application is already started; call start() once, instead of show()");
            if (!httpDocRoot.empty() && !developerEnvironment)
                detail::noteDevelopmentPath();
            startServerThread();
            try {
                if constexpr (secure)
                    Frontend::launchHttps(httpAddress, httpPort, page);
                else if constexpr (requires { Frontend::launch(httpAddress, httpPort, page, contentPolicy); })
                    Frontend::launch(httpAddress, httpPort, page, contentPolicy);
                else
                    Frontend::launch(httpAddress, httpPort, page);
            } catch (...) {
                stopAndJoinServer();  // stopped for good: the server is not re-armed, so `launched` stays set
                throw;
            }
        }
    }

    /** @brief Asks the interface that show() is running to end, as if the user had closed the window; show() then
     * returns on its own thread. Callable from any thread while the application lives, never blocking.
     *
     * Returns false when the request reached nothing: this application is not showing a window (the frontend's
     * session, if any, belongs to another application), the frontend has no window of its own to close (the system
     * browser), no window is open yet, or it already closed. A caller that must end a session whose window may still be
     * opening repeats the request until show() has returned, as webfront::UIThread::stop() does.
     */
    bool requestClose() {
        if constexpr (frontend::CanRequestClose<Frontend>) {
            // The frontend is process-wide: a window it shows may belong to another application. Only the show() that took
            // the session's ownership (an odd epoch) asks; and a frontend that publishes its interface thread must also
            // be running a session on that show()'s thread. A second open() refused for another session owns nothing.
            const auto epoch = showEpoch.load();
            if (epoch % 2 == 0)
                return false;
            if constexpr (requires {
                              { Frontend::uiExecutor() } -> std::same_as<frontend::UIExecutor>;
                          }) {
                bool publishesSessions = true;  // false: the frontend in use has no executor (an empty handle), so no session to name
                if constexpr (requires {
                                  { Frontend::publishesUIExecutor() } -> std::same_as<bool>;
                              })
                    publishesSessions = Frontend::publishesUIExecutor();
                if (publishesSessions) {
                    const auto shows   = showThread.load();
                    const auto session = Frontend::uiExecutor().session();
                    // The same epoch after the reads: this show() owned the session throughout, and its thread is busy
                    // inside open(), so a session on that thread is this application's.
                    if (!session || session->thread != shows || showEpoch.load() != epoch)
                        return false;
                    // Another application may open a window between this check and the toolkit call: a frontend that
                    // takes the session checks it again where the request runs, so that it never closes a later one.
                    if constexpr (requires {
                                      { Frontend::requestClose(session) } -> std::same_as<bool>;
                                  })
                        return Frontend::requestClose(session);
                }
            }
            if (showEpoch.load() != epoch)
                return false;
            if constexpr (requires {
                              { Frontend::requestClose() } -> std::same_as<bool>;
                          })
                return Frontend::requestClose();
            else
                return Frontend::requestClose(std::nullopt);  // only the form that takes a session, and none is published
        } else {
            return false;
        }
    }

    /// Whether the frontend type can show a second window of the application (see openSecondaryWindow()). A frontend that
    /// chooses at run time, as NativeOrBrowserFrontend does, may still refuse it: canShowSecondaryWindow() tells.
    static constexpr bool supportsSecondaryWindow =
        requires(std::string_view text, const ContentPolicy& policy, const WindowOptions& window, const ViewLaunch& launch) {
            Frontend::openSecondaryWindow(text, text, text, policy, window, launch);
            { Frontend::uiExecutor() } -> std::same_as<frontend::UIExecutor>;
        };

    /** @brief Whether openSecondaryWindow() can open a window with the frontend chosen at run time, once show() runs: false
     * when the frontend type cannot (supportsSecondaryWindow), and for NativeOrBrowserFrontend once it fell back to the system
     * browser (the frontend is initialized when the first application is constructed). Use it to offer the action or not.
     */
    [[nodiscard]] static bool canShowSecondaryWindow() {
        if constexpr (!supportsSecondaryWindow)
            return false;
        else if constexpr (requires {
                               { Frontend::secondaryWindowsAvailable() } -> std::convertible_to<bool>;
                           })
            return Frontend::secondaryWindowsAvailable();
        else
            return true;
    }

    /** @brief Opens `page` in a second native window beside the one show() runs, served by the same server (#616).
     *
     * The page reaches the same `cppFunction`s and the same `jsFunction` subscribers as the first one (each window is
     * one more connected browser: `onUIStarted` and `subscribeUIStarted` see it), and receives the same view-association
     * proof, so it is an embedded view of this application and not an external browser. The title, the size and the
     * background come from `options` (an empty background takes the application's). Opening a page that a secondary
     * window already shows brings that window to the front instead of opening another. A window is identified by the page
     * string exactly as given, query included: "a.html?x=1" and "a.html?x=2" are two windows, and requestClose(page) takes
     * the string that opened it. Closing the window of show() closes the others with it; closing a secondary window leaves
     * the rest running. A window that cannot start (its view refuses the navigation) throws the failure to the caller; a page
     * that fails to load later closes its window and the failure is written to stderr: this call has returned by then, so
     * nothing can throw it. The window follows the first one's visibility: a hidden application shows none.
     *
     * Callable from any thread while show() runs, and it waits for the interface thread: it returns once the window exists
     * (on the interface thread itself it runs at once). Called from a `cppFunction` callback, it holds the server's thread
     * until then, and with it every exchange of the application, also while a modal dialog keeps the interface thread busy;
     * there, run it on the interface thread instead (an operation scheduled with `app.uiExecutor()`), where it does not wait.
     * Under WebView2, a call made just after show() started waits until the first window's browser is created; on the
     * interface thread that call throws std::logic_error instead. Throws std::logic_error when this application is not
     * showing a window (also once the first window has closed), and when the frontend cannot show a second window (today the
     * WebKitGTK, WKWebView and WebView2 frontends can; the system browser and CEF refuse; canShowSecondaryWindow() tells it
     * beforehand). Window placement is left to the toolkit.
     */
    void openSecondaryWindow(std::string_view page, WindowOptions options = {}) {
        if constexpr (supportsSecondaryWindow) {
            // One read: its parity and the session this call is bound to are those of the same instant. A second read could
            // land in a show() that started after the first one ended, and bind the call to a session it was not made for.
            const auto epoch = showEpoch.load();
            if (epoch % 2 == 0)
                throw std::logic_error("webfront::BasicWF::openSecondaryWindow(): this application is not showing a window; call show() first");
            const auto executor = Frontend::uiExecutor();
            const auto session  = executor.session();
            if (!session || session->thread != showThread.load() || showEpoch.load() != epoch)
                throw std::logic_error("webfront::BasicWF::openSecondaryWindow(): this application has no active interface session");
            if (options.background.empty())
                options.background = windowOptions.background;
            try {
                // Called on the interface thread, the frontend cannot wait for its first window to become ready: a caller on
                // another thread tries again while it is not (WebView2 just after show() started).
                frontend::invokeOnUIThreadWhenReady(executor, [&] {
                    if (showEpoch.load() != epoch || executor.session() != session)
                        throw std::logic_error("webfront::BasicWF::openSecondaryWindow(): the interface session has ended");
                    Frontend::openSecondaryWindow(httpAddress,
                                                  httpPort,
                                                  page,
                                                  contentPolicy,
                                                  options,
                                                  ViewLaunch{viewSecret, detail::browserOrigin(httpAddress, httpPort, networking::secureTransport<Net>)});
                });
            } catch (const frontend::UIExecutorUnavailable&) {
                throw std::logic_error("webfront::BasicWF::openSecondaryWindow(): the interface session has ended");
            }
        } else {
            throw std::logic_error("webfront::BasicWF::openSecondaryWindow(): this frontend cannot show a second window of the application");
        }
    }

    /** @brief Asks the secondary window showing `page` to close, as if the user had closed it; callable from any thread.
     * Returns false when no such window is open (or the frontend has none). The window of show() ends with
     * requestClose(). Unlike requestClose(), it waits for the interface thread, as openSecondaryWindow() does: from a
     * `cppFunction` callback, run it on the interface thread instead.
     */
    bool requestClose(std::string_view page) {
        if constexpr (supportsSecondaryWindow && requires {
                          { Frontend::closeSecondaryWindow(page) } -> std::same_as<bool>;
                      }) {
            const auto epoch = showEpoch.load();
            if (epoch % 2 == 0)
                return false;
            const auto executor = Frontend::uiExecutor();
            const auto session  = executor.session();
            if (!session || session->thread != showThread.load() || showEpoch.load() != epoch)
                return false;
            bool closed = false;
            try {
                frontend::invokeOnUIThread(executor, [&] {
                    if (showEpoch.load() == epoch && executor.session() == session)
                        closed = Frontend::closeSecondaryWindow(page);
                });
            } catch (const frontend::UIExecutorUnavailable&) {
                return false;
            }
            return closed;
        } else {
            return false;
        }
    }

protected:
    /// For the developer application: its document root is the project that wf builds, which is the development path.
    void useDeveloperEnvironment() noexcept {
        developerEnvironment = true;
        // The environment's own dark theme under its pages: without a colour of the application's, an embedded view shows its
        // toolkit's white between the bootstrap page and the project's page (a flash on every navigation).
        if (windowOptions.background.empty())
            windowOptions.background = "#141821";
    }

    /// The table of the watches, null until the developer bootstrap enabled it.
    [[nodiscard]] std::shared_ptr<inspect::WatchRegistry> inspectorRegistry() const {
        std::scoped_lock lock{watchRegistryMutex};
        return watchRegistry;
    }

    /// Whether the application chose WebFrontConfig::trustLoopbackClients: a new connection that is not proven would then
    /// be a local client when its peer is the loopback (the inspectors' containers, ADR-027 section 8).
    [[nodiscard]] bool trustsLoopbackClients() const noexcept {
        return trustLoopbackClients;
    }

private:
    /// The run of show() that owns the interface: its epoch and the interface loop's session, empty when none runs. Read when a
    /// request is admitted (DeveloperAppWithNetwork, a friend), then checked on the interface thread with isCurrentShowSession():
    /// a request admitted for one run never opens or closes a window of the next one.
    struct ShowSession {
        std::uint64_t                      epoch{};
        std::optional<frontend::UISession> session;
    };
    [[nodiscard]] ShowSession currentShowSession() const {
        if constexpr (supportsSecondaryWindow) {
            const auto epoch = showEpoch.load();
            if (epoch % 2 == 0)
                return {};
            auto session = Frontend::uiExecutor().session();
            if (!session || session->thread != showThread.load() || showEpoch.load() != epoch)
                return {};
            return {epoch, std::move(session)};
        } else {
            return {};
        }
    }
    [[nodiscard]] bool isCurrentShowSession(const ShowSession& bound) const {
        if (!bound.session)
            return false;
        const auto now = currentShowSession();
        return now.epoch == bound.epoch && now.session == bound.session;
    }

    /// For the developer environment's bootstrap only (DeveloperAppWithNetwork, a friend): from now on, watch() registers in
    /// the table that the inspectors read (ADR-027). Returns that table. Private, so that no other subclass gives watch() a
    /// table without the bootstrap or in a distribution build, where the handle must stay empty (section 1).
    std::shared_ptr<inspect::WatchRegistry> enableInspectors() {
        std::scoped_lock lock{watchRegistryMutex};
        if (!watchRegistry) {
            watchRegistry             = std::make_shared<inspect::WatchRegistry>();
            watchRegistry->diagnostic = [](std::string_view message) {
                log::warn("{}", message);
            };
        }
        if (jsCalls)
            jsCalls->enable();
        return watchRegistry;
    }

    /// What the inspectors keep for the connection of `caller` (its memory of each watch), freed when it closes. Throws
    /// when the connection is gone.
    [[nodiscard]] std::shared_ptr<inspect::WatchSession> inspectorSession(const UI& caller) const {
        const auto registry   = inspectorRegistry();
        const auto generation = deferredCallsOf(caller).second;
        if (!registry || generation == 0)
            throw detail::makeBridgeError(bridge_code::disconnected, "The connection is closed");
        return registry->session(generation);
    }

    /// Frees what the inspectors keep for a connection that closed.
    void forgetInspectorSession(WebLinkGeneration generation) const noexcept {
        if (const auto registry = inspectorRegistry())
            registry->forget(generation);
    }

    /// For the inspectors' functions (DeveloperAppWithNetwork): what cppFunction registered, and the calls made to JavaScript.
    [[nodiscard]] const std::shared_ptr<inspect::FunctionRegistry>& inspectedFunctions() const noexcept {
        return functionRegistry;
    }
    [[nodiscard]] std::shared_ptr<inspect::JsCallRegistry> inspectedJsCalls() const {
        return jsCalls ? jsCalls->registry() : nullptr;
    }

    /// The browsers connected now, in the order of their identifiers, as the inspectors show them; `caller` is the one that asks.
    [[nodiscard]] std::vector<inspect::BrowserInfo> inspectedBrowsers(const UI& caller) const {
        return inspectedBrowsers(&caller);
    }
    /// The same when native code asks (`caller` null): no browser is the caller.
    [[nodiscard]] std::vector<inspect::BrowserInfo> inspectedBrowsers(const UI* caller) const {
        struct Seen {
            inspect::BrowserInfo                   info;
            std::shared_ptr<WebLink<Net, Policy>>  link;
            std::shared_ptr<detail::DeferredCalls> calls;
        };
        std::vector<Seen> seen;
        {
            std::scoped_lock lock{linksMutex};
            seen.reserve(webLinks.size());
            for (const auto& [id, connection] : webLinks) {
                inspect::BrowserInfo info;
                info.id           = id;
                info.embeddedView = !connection.closed && connection.viewAssociated;
                info.localClient  = !connection.closed && (connection.viewAssociated || (trustLoopbackClients && connection.loopbackPeer));
                info.started      = connection.stage == Connection::Stage::started;
                info.closed       = connection.closed;
                info.caller       = caller != nullptr && *caller == UI{const_cast<BasicWF&>(*this), id, connection.generation};
                info.path         = connection.path;
                info.space        = connection.space ? connection.space->path : std::string{};
                seen.push_back({std::move(info), connection.link, connection.deferredCalls});
            }
        }
        // Outside linksMutex: each count takes the lock of what it counts.
        std::vector<inspect::BrowserInfo> browsers;
        browsers.reserve(seen.size());
        for (auto& [info, link, calls] : seen) {
            if (link) {
                info.pendingWrites  = link->pendingWrites();
                info.pendingResults = link->pendingResultCount();
            }
            info.pendingCalls = calls ? calls->pending() : 0;
            browsers.push_back(std::move(info));
        }
        return browsers;
    }

    /// The tasks of the server's scope that have not ended: connections, bridge loops and the application's coroutines.
    [[nodiscard]] std::size_t inspectedTasks() const noexcept {
        return httpServer.taskScope().activeTasks();
    }

    // The constructor has already bound and listened; this thread services the queued I/O.
    // Launch server only once or restart if previous thread ended
    void startServerThread() {
        if (!serverThread.joinable()) {
            serverThread = std::thread([this] {
                run();
            });
        }
    }

    friend struct pty::detail::PageRegistration;
    std::vector<std::function<void()>> ptyPageCleanups;

    void closePtyPages() {
        for (const auto& cleanup : ptyPageCleanups)
            cleanup();
        ptyPageCleanups.clear();
    }

    void stopAndJoinServer() {
        stop();
        if (serverThread.joinable())
            serverThread.join();
    }

    /// forEachUI, restricted to the browsers of the space `only` when it is given.
    template <typename Function>
    void forEachUIIn(const SpaceState* only, Function&& function) {
        std::vector<UI> started;
        {
            std::scoped_lock lock{linksMutex};
            for (const auto& [id, connection] : webLinks)
                if (!connection.closed && connection.stage == Connection::Stage::started && (!only || connection.space.get() == only))
                    started.emplace_back(*this, id, connection.generation);
        }
        for (const auto& ui : started)
            std::invoke(function, ui);
    }

    [[nodiscard]] std::size_t browsersIn(const SpaceState* space) const {
        std::scoped_lock lock{linksMutex};
        return static_cast<std::size_t>(std::ranges::count_if(webLinks, [space](const auto& entry) {
            return !entry.second.closed && entry.second.space.get() == space;
        }));
    }

    /// Registers the space `segment` of the family `prefix` (BasicSpace's constructor).
    std::shared_ptr<SpaceState> attachSpace(std::string_view prefix, std::string_view segment) {
        if (!detail::validSpaceSegment(segment))
            throw std::invalid_argument("'" + std::string(segment)
                                        + "' cannot name a space: it is empty or '.', or holds '..', '/', '\\' or a control character");
        std::scoped_lock lock{spaceRegistry->mutex};
        if (std::ranges::find(spaceRegistry->families, prefix) == spaceRegistry->families.end())
            throw std::invalid_argument("No family of spaces was declared with the prefix '" + std::string(prefix) + "': call spaces() first");
        auto path = std::string(prefix) + std::string(segment) + "/";
        if (spaceRegistry->spaces.contains(path))
            throw std::logic_error("The space '" + path + "' exists already");
        auto state = std::make_shared<SpaceState>(path, std::string(segment));
        spaceRegistry->spaces.emplace(std::move(path), state);
        return state;
    }

    /// Closes the pages of a space being destroyed, already removed from the registry, and releases what it registered.
    /// Its pages are cut at once (no call reaches a function any more, no broadcast reaches them); their connections are
    /// released on the server context, where the application's closed handlers are told.
    void detachSpace(const std::shared_ptr<SpaceState>& space) {
        std::vector<std::pair<WebLinkId, WebLinkGeneration>> pages;
        std::vector<std::shared_ptr<WebLink<Net, Policy>>>   links;
        {
            std::scoped_lock lock{linksMutex};
            space->alive = false;
            for (auto& [id, connection] : webLinks)
                if (connection.space == space && !connection.closed) {
                    connection.closed = true;
                    connection.deferred.clear();
                    pages.emplace_back(id, connection.generation);
                    links.push_back(connection.link);
                }
        }
        // Route handlers run on the server's workers, not on this thread: the space waits for those running, and none starts.
        space->routeGuard->retire();
        space->release();
        for (const auto& link : links)
            link->stop();  // a stopped link reports no closure: closeSpacePages does what the closed event would
        if (!pages.empty())
            static_cast<void>(httpServer.startTask(closeSpacePages(std::move(pages))));  // refused during shutdown, which releases them
    }

    Task closeSpacePages(std::vector<std::pair<WebLinkId, WebLinkGeneration>> pages) {
        for (const auto& [id, generation] : pages) {
            std::shared_ptr<detail::DeferredCalls> stoppedCalls;
            {
                std::scoped_lock lock{linksMutex};
                const auto       found = webLinks.find(id);
                if (found == webLinks.end() || found->second.generation != generation)
                    continue;
                stoppedCalls = found->second.deferredCalls;
            }
            stoppedCalls->close();
            readBinding->close(generation);
            forgetInspectorSession(generation);
            if (auto found = domDocuments.find(id); found != domDocuments.end()) {
                found->second->close();
                domDocuments.erase(found);
            }
            uiClosedSubscribers->notify(UI{*this, id, generation});
            std::shared_ptr<WebLink<Net, Policy>> released;  // destroyed outside the lock
            std::scoped_lock                      lock{linksMutex};
            if (auto found = webLinks.find(id); found != webLinks.end() && found->second.generation == generation) {
                released = std::move(found->second.link);
                webLinks.erase(found);
            }
        }
        co_return;
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
        readBinding->shutdown();  // read URLs die with the application, and no grant is issued afterwards
        for (auto& [id, document] : domDocuments)
            document->close();
        domDocuments.clear();
        std::map<WebLinkId, Connection> released;
        {
            std::scoped_lock lock{linksMutex};
            released.swap(webLinks);
        }
        // Their closure is not reported during shutdown: what the inspectors kept for them is freed here.
        for (const auto& [id, connection] : released)
            forgetInspectorSession(connection.generation);
    }

    /// The type-erased handler of a C++ function, as cppFunction<R, Args...> registers it.
    template <typename R, typename... Args>
    CppHandler makeCppHandler(auto&& function, std::shared_ptr<inspect::FunctionRecord> record = {}) {
        if constexpr (detail::deferred_result<R>)
            return makeDeferredHandler<R, Args...>(std::forward<decltype(function)>(function), std::move(record));
        else if constexpr (detail::takes_caller<UI, Args...>::value)
            return makeCallerHandler<R, Args...>(std::forward<decltype(function)>(function), std::move(record));
        else {
            auto callable  = detail::makeCppFunctionHandler<Policy, R, Args...>(std::forward<decltype(function)>(function));
            auto responder = detail::makeCppFunctionResponder<Net, Policy, R>(std::move(callable), std::move(record));
            return [responder](std::span<const std::byte> data, WebLink<Net, Policy>& link, msg::CallId callId, UI) mutable {
                responder(data, link, callId);
            };
        }
    }

    /// A callback that returns a sender (a ResultTask<R> among them) answers when it completes, not when it returns: the
    /// call is owned by the connection (its closure stops the work and drops the late answer) and by the server scope
    /// (shutdown stops and joins it). The answer is sent on the server context.
    template <typename R, typename... Args>
    CppHandler makeDeferredHandler(auto&& function, std::shared_ptr<inspect::FunctionRecord> record) {
        // Naming the value type checks the contract at registration: one value at most, one kind of value completion.
        using DeferredValue [[maybe_unused]] = detail::deferred_value_t<R>;
        const auto start                     = [this, record = std::move(record)](auto invoke, WebLink<Net, Policy>& link, msg::CallId callId, UI caller) {
            this->startDeferred(std::move(invoke), link, callId, caller, record);
        };
        if constexpr (detail::takes_caller<UI, Args...>::value) {
            auto handler = registerDeferredHandler<Args...>(std::forward<decltype(function)>(function));
            return [start, handler](std::span<const std::byte> data, WebLink<Net, Policy>& link, msg::CallId callId, UI caller) mutable {
                start(
                    [&] {
                        return handler(data, caller);
                    },
                    link,
                    callId,
                    caller);
            };
        } else {
            auto handler = detail::makeDeferredHandler<Policy, Args...>(std::forward<decltype(function)>(function));
            return [start, handler](std::span<const std::byte> data, WebLink<Net, Policy>& link, msg::CallId callId, UI caller) mutable {
                start(
                    [&] {
                        return handler(data);
                    },
                    link,
                    callId,
                    caller);
            };
        }
    }

    template <typename Caller, typename... Args>
    auto registerDeferredHandler(auto&& function) {
        return detail::makeDeferredHandler<Policy, Args...>(detail::adaptCaller<Caller>(std::forward<decltype(function)>(function)));
    }

    /// Claims the call identifier, then calls the callback and hands the sender it returned to a task that answers when it
    /// completes. The claim comes first: a call refused as a duplicate, or on a closed connection, never runs the callback.
    template <typename Invoke>
    void startDeferred(Invoke invoke, WebLink<Net, Policy>& link, msg::CallId callId, UI caller, const std::shared_ptr<inspect::FunctionRecord>& record) {
        const auto [calls, generation] = deferredCallsOf(caller);
        const auto key                 = calls ? calls->reserve(callId) : 0;
        if (key == 0) {
            // The connection closed (nobody is left to answer), or a call with this identifier is still running: the common
            // reception drops such a call before it gets here, and this claim is the last guard.
            return;
        }
        // The call is counted now and timed until its answer: the measure goes with the task that answers.
        // It holds the record, not the registration: a space that releases the function meanwhile unlists it at once.
        inspect::CallMeasure                                   measure{record ? record->keep() : nullptr};
        std::optional<std::remove_cvref_t<decltype(invoke())>> sender;
        try {
            sender.emplace(invoke());
        } catch (const std::exception& error) {
            measure.failed(error.what());
            calls->finish(callId, key);
            if (callId != 0)
                link.sendFrame(detail::makeFailureFrame<Net, Policy>(callId, error));
            return;
        } catch (...) {
            measure.failed("Unknown C++ exception");
            calls->finish(callId, key);
            if (callId != 0)
                link.sendFrame(detail::makeFailureFrame<Net, Policy>(callId, std::runtime_error{"Unknown C++ exception"}));
            return;
        }
        const auto handle = spawn(respondLater(caller, generation, calls, callId, key, std::move(*sender), std::move(measure)));
        if (handle.stopRequested()) {
            // The server refused the task (it is shutting down): it never runs, so nothing would release the identifier.
            calls->finish(callId, key);
            return;
        }
        calls->attach(key, handle);
    }

    [[nodiscard]] bool callInFlight(WebLinkId id, WebLinkGeneration generation, msg::CallId callId) const {
        std::shared_ptr<detail::DeferredCalls> calls;
        {
            std::scoped_lock lock{linksMutex};
            const auto       found = webLinks.find(id);
            if (found == webLinks.end() || found->second.generation != generation)
                return false;
            calls = found->second.deferredCalls;
        }
        return calls->inFlight(callId);
    }

    std::pair<std::shared_ptr<detail::DeferredCalls>, WebLinkGeneration> deferredCallsOf(const UI& caller) const {
        std::scoped_lock lock{linksMutex};
        const auto       found = webLinks.find(caller.id());
        if (found == webLinks.end() || found->second.closed)
            return {nullptr, WebLinkGeneration{0}};
        return {found->second.deferredCalls, found->second.generation};
    }

    /// Awaits the sender of a deferred callback and sends its result, its failure, or `webfront.cancelled` when it
    /// completed stopped by something other than this call's own stop request. When that request was made (the
    /// connection closed, the application is stopping) nothing is sent: nobody is left to read it.
    template <typename Sender>
    Task respondLater(UI                                     caller,
                      WebLinkGeneration                      generation,
                      std::shared_ptr<detail::DeferredCalls> calls,
                      msg::CallId                            callId,
                      std::uint64_t                          key,
                      Sender                                 sender,
                      inspect::CallMeasure                   measure) {
        struct Finish {
            detail::DeferredCalls& calls;
            msg::CallId            callId;
            std::uint64_t          key;
            ~Finish() {
                calls.finish(callId, key);
            }
        } finish{*calls, callId, key};
        const auto                           token = co_await networking::ex::read_env(networking::ex::get_stop_token);
        std::optional<websocket::Frame<Net>> answer;
        try {
            auto stoppable = networking::ex::stopped_as_error(
                std::move(sender),
                std::make_exception_ptr(detail::makeBridgeError(bridge_code::cancelled, "The C++ callback was cancelled")));
            // One answer frame whether the sender completes with a value or without: the message is built where it completes.
            auto framed = networking::ex::then(std::move(stoppable), [callId](auto&&... values) -> websocket::Frame<Net> {
                if constexpr (sizeof...(values) == 0)
                    return detail::makeVoidFrame<Net, Policy>(callId);
                else
                    // A value that cannot be encoded throws here: the call then fails as its callback's exception would,
                    // and is counted so, as on the path of a callback that answers at once.
                    return detail::encodeValueFrame<Net, Policy>(callId, values...);
            });
            answer.emplace(co_await std::move(framed));
        } catch (const std::exception& error) {
            // A call stopped by its browser's disconnection or by the shutdown is a failure too: it gave no result.
            measure.failed(error.what());
            answer.emplace(detail::makeFailureFrame<Net, Policy>(callId, error));
        } catch (...) {
            measure.failed("Unknown C++ exception");
            answer.emplace(detail::makeFailureFrame<Net, Policy>(callId, std::runtime_error{"Unknown C++ exception"}));
        }
        measure = {};  // the call ends with its answer, not with this task
        if (token.stop_requested() || callId == 0)
            co_return;
        if (const auto link = findLink(caller.id(), generation))
            link->sendFrame(std::move(*answer));
    }

    template <typename R, typename Caller, typename... Args>
    CppHandler makeCallerHandler(auto&& function, std::shared_ptr<inspect::FunctionRecord> record) {
        auto adapted   = detail::adaptCaller<Caller>(std::forward<decltype(function)>(function));
        auto callable  = detail::makeCppFunctionHandler<Policy, R, Args...>(std::move(adapted));
        auto responder = detail::makeCppFunctionResponder<Net, Policy, R>(std::move(callable), std::move(record));
        return [responder](std::span<const std::byte> data, WebLink<Net, Policy>& link, msg::CallId callId, UI caller) mutable {
            responder(data, link, callId, caller);
        };
    }

    // Before the server: the routes' handlers use it, and the server drains them before it is destroyed.
    std::shared_ptr<ReadBinding>            readBinding;
    http::Server<Net, Filesystem, Policy>   httpServer;
    http::RouteTable::Registration          bindRoute;
    std::string                             httpAddress;
    std::string                             httpPort;
    std::filesystem::path                   httpDocRoot;
    ContentPolicy                           contentPolicy;
    WindowOptions                           windowOptions;
    const std::string                       viewSecret = detail::newViewSecret();  // this launch's proof of an embedded view
    const bool                              trustLoopbackClients;
    const bool                              ptyPagesAllowed;
    bool                                    developerEnvironment = false;
    mutable std::mutex                      watchRegistryMutex;
    std::shared_ptr<inspect::WatchRegistry> watchRegistry;  // set once by enableInspectors(), read by watch() from any thread
    // What cppFunction registered (ADR-027, section 7); shared with the records' owners, which may outlive a registration.
    std::shared_ptr<inspect::FunctionRegistry> functionRegistry = std::make_shared<inspect::FunctionRegistry>();
    // The counters of the calls to JavaScript, empty until enableInspectors(); null in a distribution build. Never replaced.
    const std::shared_ptr<inspect::JsCallSource> jsCalls = inspect::introspection ? std::make_shared<inspect::JsCallSource>() : nullptr;
    std::atomic<bool>                            launched{false};     // start() was called
    std::atomic<std::thread::id>                 showThread{};        // the thread inside this application's Frontend::open(), if any
    std::atomic<std::uint64_t>                   showEpoch{0};        // odd while show() runs its frontend, changes at each entry and exit
    std::atomic<bool>                            showRunning{false};  // a show() of this application is in progress
    std::atomic<bool>                            shown{false};        // show() was called, whether or not its server still runs
    mutable std::mutex                           linksMutex;          // guards webLinks, read from any thread
    struct Connection {
        std::shared_ptr<WebLink<Net, Policy>> link;
        WebLinkGeneration                     generation;
        std::thread::id                       thread;
        bool                                  closed;
        enum class Stage { handshaking, starting, started };
        Stage                                   stage = Stage::handshaking;  // starting while its onUIStarted handlers run
        std::vector<detail::BroadcastCall<Net>> deferred;                    // sendToStartedLinks() calls while starting
        bool                                    viewAssociated = false;      // presented the launch secret of an embedded view
        bool                                    loopbackPeer   = false;      // peer address was the loopback (read only with trustLoopbackClients)
        std::shared_ptr<detail::DeferredCalls>  deferredCalls  = std::make_shared<detail::DeferredCalls>();  // calls a callback answers later
        std::shared_ptr<SpaceState>             space{};  // the space the page was loaded in; null outside the spaces
        std::string                             path{};   // the path of the upgrade, within the space (UI::path)
    };
    std::map<WebLinkId, Connection>                webLinks;
    WebLinkId                                      idsCounter{0};
    WebLinkGeneration                              generationCounter{0};  // guarded by linksMutex; the first connection is 1
    std::function<void(UI)>                        uiStartedHandler;
    std::shared_ptr<detail::UISubscribers<UI>>     uiSubscribers       = std::make_shared<detail::UISubscribers<UI>>();
    std::shared_ptr<detail::UISubscribers<UI>>     uiClosedSubscribers = std::make_shared<detail::UISubscribers<UI>>();
    std::map<std::string, CppHandler, std::less<>> cppFunctions;
    std::shared_ptr<SpaceRegistryType>             spaceRegistry = std::make_shared<SpaceRegistryType>();
    // Keyed by identifier alone: a document is closed and erased on its connection's closed event, before the link is
    // released and its identifier can be reused, so two connections never share an entry.
    std::map<WebLinkId, std::shared_ptr<dom::detail::State>> domDocuments;
    std::thread                                              serverThread;  // Background thread running the HTTP server

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
                std::optional<WebLinkGeneration> bindKey;
                bool                             local = false;
                std::shared_ptr<SpaceState>      space;
                {
                    std::scoped_lock lock{linksMutex};
                    if (auto found = webLinks.find(event.webLinkId); found != webLinks.end()) {
                        found->second.stage = Connection::Stage::starting;
                        space               = found->second.space;
                        // Raised once, never lowered: the secret is judged at the single handshake WebLink dispatches.
                        found->second.viewAssociated = found->second.viewAssociated || detail::viewSecretMatches(viewSecret, event.text);
                        bindKey                      = found->second.generation;
                        local                        = found->second.viewAssociated;  // the only local transport (ADR-020, rule 3)
                    }
                }
                // Even when a handler throws: the browser must not keep collecting deferred sends.
                struct Started {
                    BasicWF*  app;
                    WebLinkId id;
                    ~Started() {
                        app->finishStarting(id);
                    }
                } started{this, event.webLinkId};
                const auto ui = uiOf(event.webLinkId);
                if (bindKey) {
                    // The page swaps this one-time secret for the cookie that binds its reads to its browser client.
                    try {
                        ui.jsFunction("wfBind.exchange")(readBinding->open(*bindKey, local));
                    } catch (const std::exception& error) {
                        log::error("The read binding could not be offered to a browser: {}", error.what());
                    }
                }
                if (uiStartedHandler)
                    uiStartedHandler(ui);
                uiSubscribers->notify(ui);
                if (space && space->alive) {
                    if (const auto handler = space->onStarted())
                        (*handler)(ui);  // the handler that was set, not a copy: its state persists
                    // A handler may destroy the space: the space's handlers that follow it are then not called.
                    space->started->notifyWhile(ui, [&space] {
                        return space->alive.load();
                    });
                }
                break;
            }
            case WebLinkEvent::Code::closed: {
                // Shutdown cancels the connections' writes, which then fail and report a closure: not a disconnection.
                if (httpServer.taskScope().stopRequested())
                    break;
                std::optional<WebLinkGeneration>       closedGeneration;
                std::shared_ptr<detail::DeferredCalls> stoppedCalls;
                std::shared_ptr<SpaceState>            space;
                bool                                   spaceEmptied = false;
                {
                    std::scoped_lock lock{linksMutex};
                    if (auto found = webLinks.find(event.webLinkId); found != webLinks.end()) {
                        space = found->second.space;
                        if (space && !found->second.closed)
                            spaceEmptied = --space->pages == 0 && space->alive;
                        found->second.closed = true;
                        found->second.deferred.clear();
                        closedGeneration = found->second.generation;
                        stoppedCalls     = found->second.deferredCalls;
                    }
                }
                // The calls a callback has not answered are stopped and their late answers dropped, outside the lock: a
                // stop callback runs on this thread.
                if (stoppedCalls)
                    stoppedCalls->close();
                // Outside linksMutex: dropping the grants destroys their contexts, whose destructors may use the application.
                if (closedGeneration)
                    readBinding->close(*closedGeneration);  // its read URLs stop working at once
                if (closedGeneration)
                    forgetInspectorSession(*closedGeneration);
                if (auto found = domDocuments.find(event.webLinkId); found != domDocuments.end()) {
                    found->second->close();
                    domDocuments.erase(found);
                }
                const auto ui = uiOf(event.webLinkId);
                if (space)
                    space->closed->notifyWhile(ui, [&space] {
                        return space->alive.load();
                    });
                uiClosedSubscribers->notify(ui);
                if (spaceEmptied && space->alive) {
                    // The handler may destroy the space: it runs on a copy, and the state is kept by `space`.
                    if (const auto handler = space->onLastClosed())
                        (*handler)();
                }
                // The link is running this handler: it is released afterwards, by a task on the server context.
                // During shutdown the task is refused and the link goes with the application.
                static_cast<void>(httpServer.startTask(releaseLink(event.webLinkId)));
                break;
            }
            case WebLinkEvent::Code::cppFunctionCalled: {
                // Links are registered before they read, and released after their closure: the caller is registered.
                std::shared_ptr<WebLink<Net, Policy>> link;
                WebLinkGeneration                     generation{0};
                std::shared_ptr<SpaceState>           space;
                {
                    std::scoped_lock lock{linksMutex};
                    const auto       found = webLinks.find(event.webLinkId);
                    // A page of a destroyed space is closed before its link is released: its calls reach nothing.
                    if (found == webLinks.end() || found->second.closed)
                        break;
                    link       = found->second.link;
                    generation = found->second.generation;
                    space      = found->second.space;
                }
                // A browser that reuses the identifier of a call a callback has not answered yet breaks the protocol (its own
                // calls never do). Whatever the function, no second call and no answer with that identifier: an answer could
                // settle the first call's Promise, and the first call's own answer a later call that reuses the identifier.
                if (callInFlight(event.webLinkId, generation, event.callId)) {
                    log::error("Ignoring a call to '{}' that reuses the identifier {} of a call still running", event.text, event.callId);
                    break;
                }
                if constexpr (requires(typename Net::Acceptor& acceptor) { Net::ResumeAfter(acceptor, std::chrono::milliseconds(1)); }) {
                    if (domCall(event, *link))
                        break;
                } else if (event.text == "wfDom.ready") {
                    link->sendError(event.callId, "DOM API requires Net::ResumeAfter");
                    break;
                }
                // The space's own functions first, then the application's, which every space sees.
                if (const auto own = space ? space->function(event.text) : nullptr)
                    (*own)(event.data, *link, event.callId, UI{*this, event.webLinkId, generation});
                else if (auto function = cppFunctions.find(event.text); function != cppFunctions.end())
                    function->second(event.data, *link, event.callId, UI{*this, event.webLinkId, generation});
                else
                    detail::rejectWithBridgeCode<Net, Policy>(*link, event.callId, bridge_code::notFound, "C++ function '" + event.text + "' was not found");
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
using UI                   = WebFront::UI;

#ifdef WEBFRONT_EMBED_CEF
inline constexpr bool cefAvailable = true;
#else
inline constexpr bool cefAvailable = false;
#endif

/**
 * @brief True when at least one embedded engine is compiled in: the platform's native web view (WEBFRONT_EMBED_WEBKITGTK,
 * WEBFRONT_EMBED_WEBVIEW2 or WEBFRONT_EMBED_WKWEBVIEW) or CEF (WEBFRONT_EMBED_CEF). Always defined, so applications need no
 * preprocessor test. Like frontend::nativeWebViewAvailable, it says nothing about the session (a missing display still fails at
 * run time).
 */
inline constexpr bool embeddedFrontendAvailable = frontend::nativeWebViewAvailable || cefAvailable;

namespace detail {
/// Stands in where no engine is compiled in: constructing the application throws std::runtime_error.
struct NoEmbeddedFrontend {
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};

    static void initialize() {
        throw std::runtime_error("no embedded web view engine in this build: configure with -DWEBFRONT_EMBED_WEBKITGTK=ON (Linux), "
                                 "-DWEBFRONT_EMBED_WEBVIEW2=ON (Windows), -DWEBFRONT_EMBED_WKWEBVIEW=ON (macOS) or -DWEBFRONT_EMBED_CEF=ON");
    }
    static void open(std::string_view, std::string_view) {
        initialize();
    }
};

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
using NativeOrAbsentFrontend = std::conditional_t<frontend::nativeWebViewAvailable, frontend::NativeWebViewFrontend, NoEmbeddedFrontend>;
#else
using NativeOrAbsentFrontend = NoEmbeddedFrontend;
#endif
// The native web view wins when both exist; CEF is the engine of a build that has no native one.
using EmbeddedFrontendType = std::conditional_t<frontend::nativeWebViewAvailable || !cefAvailable, NativeOrAbsentFrontend, frontend::CEFFrontend>;
}  // namespace detail

/**
 * @brief The application type with the embedded web view window. Always defined: without an engine in the build, constructing it
 * throws std::runtime_error, so falling back to the system browser is an ordinary try/catch.
 */
using EmbeddedWebFront = WebFrontWithFrontend<detail::EmbeddedFrontendType>;

}  // namespace webfront

// Optional templates instantiate only when the developer/native facade is selected.
#include "DeveloperApp.hpp"
// Spaces use the callable traits above; BasicWF only names BasicSpace.
#include "Space.hpp"
