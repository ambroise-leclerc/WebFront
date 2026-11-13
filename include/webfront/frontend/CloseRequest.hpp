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

namespace detail {

/// One close request: owns its operation on the executor and deletes itself when it completes, whatever the outcome.
template <typename Close>
struct WindowCloseRequest {
    struct Receiver {
        using receiver_concept = ex::receiver_tag;
        WindowCloseRequest* self;

        void set_value() noexcept {
            // On the interface thread: the window is given only while it is published.
            try {
                if (const auto handle = self->window.get())
                    self->close(*handle);
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

    NativeWindow                                                         window;
    Close                                                                close;
    ex::connect_result_t<decltype(std::declval<UIExecutor&>().schedule()), Receiver> operation;

    WindowCloseRequest(const UIExecutor& executor, NativeWindow target, Close function)
        : window(std::move(target)), close(std::move(function)), operation(ex::connect(executor.schedule(), Receiver{this})) { }
};

/// Runs `close(const NativeWindowHandle&)` on the interface thread with the published window. Returns false, having
/// done nothing, when no interface loop runs (nothing is open yet, or the window already closed); true when the request
/// was handed to the loop, which drops it if the window is not published when it runs. Callable from any thread.
template <typename Close>
bool requestWindowClose(const UIExecutor& executor, NativeWindow window, Close close) noexcept {
    if (!executor.available())
        return false;
    try {
        ex::start((new WindowCloseRequest<Close>(executor, std::move(window), std::move(close)))->operation);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace detail
}  // namespace webfront::frontend
