/// @brief Asking the window of an embedded frontend to close, from any thread (#567).
///
/// A host application that keeps its own loop ends a WebFront session with `BasicWF::requestClose()`, which forwards to the
/// frontend's optional `static bool requestClose()`. The embedded adapters implement it with `requestWindowClose`: the
/// toolkit call that closes the window runs on the interface thread (UIExecutor.hpp), with the window the frontend
/// published (NativeWindow.hpp). The window then closes as it does for the user, so `open()` returns the same way.
#pragma once

#include "NativeWindow.hpp"
#include "UIExecutor.hpp"

#include <concepts>
#include <exception>
#include <optional>
#include <utility>

namespace webfront::frontend {

/// @brief True when the frontend's `open()` belongs to the process main thread, as its optional
/// `static constexpr bool requiresMainThread` says; a frontend that does not declare it is taken to run on any thread.
template <typename Frontend>
inline constexpr bool requiresMainThread = [] {
    if constexpr (requires { { Frontend::requiresMainThread } -> std::convertible_to<bool>; })
        return static_cast<bool>(Frontend::requiresMainThread);
    else
        return false;
}();

/// @brief True when `Frontend::requestClose()` exists and can end every session the frontend opens. A frontend whose
/// session cannot always be ended that way (a fallback that waits for the console) declares
/// `static constexpr bool endsOnRequest = false`; webfront::UIThread, which must join its thread, refuses it.
template <typename Frontend>
inline constexpr bool endsOnRequest = [] {
    if constexpr (!requires { { Frontend::requestClose() } -> std::same_as<bool>; })
        return false;
    else if constexpr (requires { { Frontend::endsOnRequest } -> std::convertible_to<bool>; })
        return static_cast<bool>(Frontend::endsOnRequest);
    else
        return true;
}();

namespace detail {

/// One close request: owns its operation on the executor and deletes itself when it completes, whatever the outcome.
/// The executor completes every started operation exactly once (run, stopped when the loop closes with it pending,
/// error when no loop is attached), so the request is always freed; the unit tests check it for each outcome.
template <typename Close>
struct WindowCloseRequest {
    struct Receiver {
        using receiver_concept = ex::receiver_tag;
        WindowCloseRequest* self;

        void set_value() noexcept {
            // On the interface thread: the window is given only while it is published, and a request made for a session
            // is dropped if the loop that runs it is another one (the first closed and a second opened since).
            try {
                if (!self->session || self->executor.session() == self->session) {
                    if (const auto handle = self->window.get())
                        self->close(*handle);
                }
            } catch (...) {  // a close that cannot be made leaves the window open
            }
            delete self;
        }
        void set_error(std::exception_ptr) noexcept {
            delete self;
        }
        void set_stopped() noexcept {
            delete self;
        }
    };

    UIExecutor                                                           executor;
    NativeWindow                                                         window;
    Close                                                                close;
    std::optional<UISession>                                             session;
    ex::connect_result_t<decltype(std::declval<UIExecutor&>().schedule()), Receiver> operation;

    WindowCloseRequest(const UIExecutor& loop, NativeWindow target, Close function, std::optional<UISession> owner)
        : executor(loop), window(std::move(target)), close(std::move(function)), session(owner),
          operation(ex::connect(loop.schedule(), Receiver{this})) { }
};

/// Runs `close(const NativeWindowHandle&)` on the interface thread with the published window. Returns false, having
/// done nothing, when no interface loop runs (nothing is open yet, or the window already closed); true when the request
/// was handed to the loop, which drops it if the window is not published when it runs. Callable from any thread.
///
/// With `session` (the one `executor.session()` gave the caller), the request concerns that loop only: the check is made
/// again on the interface thread when it runs, so a window of a later session is never closed by it.
template <typename Close>
bool requestWindowClose(const UIExecutor& executor, NativeWindow window, Close close, std::optional<UISession> session = std::nullopt) noexcept {
    if (!executor.available())
        return false;
    try {
        ex::start((new WindowCloseRequest<Close>(executor, std::move(window), std::move(close), session))->operation);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace detail
}  // namespace webfront::frontend
