// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The static facade shared by the embedded frontends (#652): what a native frontend declares once, the base provides.
#pragma once

#include "../ContentPolicy.hpp"
#include "../ViewAssociation.hpp"
#include "Action.hpp"
#include "CloseRequest.hpp"
#include "NativeWindow.hpp"
#include "UIExecutor.hpp"
#include "Window.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace webfront::frontend {

/// The settings every embedded frontend takes when it opens a window; a frontend's own options start with these.
struct ViewOptions {
    ContentPolicy contentPolicy{};
    WindowOptions window{};
    ViewLaunch    launch{};
};

/**
 * @brief CRTP base of the embedded frontends: `uiExecutor()`, `nativeWindow()`, `requestClose()`, the second window, `initialize()`
 * and every overload of `open()`, from the few members the frontend declares (docs/FRONTENDS.md):
 *
 * - `using Host = ...`: the type that names the process-wide hosts (UIExecutor.hpp, NativeWindow.hpp);
 * - `static constexpr bool compiled`: the engine is part of this build; `static constexpr std::string_view name` and
 *   `unavailable` (what to configure) make the message of a frontend that is not;
 * - `using Options`: the frontend's options (`contentPolicy`, `window` and `launch` at least; ViewOptions has them), and an
 *   optional `static Options defaultOptions()` for the ones a call starts from;
 * - `static void openNative(address, port, file, const Options&)` and `closeNative(const NativeWindowHandle&)` (on the
 *   interface thread: the toolkit's own close request), and an optional `initializeNative()` (a frontend without one throws from initialize() when it is not compiled);
 * - `static constexpr bool secondaryWindows`, and when true `openSecondaryNative(address, port, page, const Options&)` and
 *   `closeSecondaryNative(page) -> bool`, which run on the interface thread; `waitsForFirstWindow` (optional) tells that
 *   the call may come before the first window is ready (invokeOnUIThreadWhenReady).
 *
 * Members that name the engine are only instantiated when `compiled` holds, so they need no `#if` of their own.
 */
template <typename Derived>
struct EmbeddedFrontend {
    static constexpr Action action{Action::closeServerAfterOpen};

    /// The interface thread while open() runs the toolkit's loop (see UIExecutor.hpp).
    [[nodiscard]] static UIExecutor uiExecutor() {
        return uiExecutorHostOf<typename Derived::Host>().executor();
    }
    /// The window, readable on the interface thread while open() runs (see NativeWindow.hpp).
    [[nodiscard]] static NativeWindow nativeWindow() {
        return nativeWindowHostOf<typename Derived::Host>().window();
    }

    static void initialize() {
        if constexpr (requires { Derived::initializeNative(); })
            Derived::initializeNative();
        else if constexpr (!Derived::compiled)
            unavailable();
    }

    /// Asks the window to close, from any thread; false when no window is open (see CloseRequest.hpp).
    static bool requestClose([[maybe_unused]] std::optional<UISession> session = std::nullopt) noexcept {
        if constexpr (Derived::compiled)
            return detail::requestWindowClose(uiExecutor(), nativeWindow(), [](const NativeWindowHandle& window) {
                Derived::closeNative(window);
            }, session);
        else
            return false;
    }

    /// Opens `page` in a second window beside the one open() runs, or brings the window already showing it to the front
    /// (#616). Callable from any thread while open() runs; throws std::logic_error when no window is open.
    static void openSecondaryWindow(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy,
                                    const WindowOptions& window, const ViewLaunch& launch)
        requires(Derived::secondaryWindows)
    {
        if constexpr (!Derived::compiled) {
            unavailable();
        } else {
            auto options = optionsOf(policy, &window, &launch);
            try {
                onUIThread([&] {
                    Derived::openSecondaryNative(address, port, page, options);
                });
            } catch (const UIExecutorUnavailable&) {
                throw std::logic_error(std::string(Derived::name) + ": no window of the application is open; show() it before opening another");
            }
        }
    }
    /// Closes the secondary window showing `page`, from any thread; false when none is open.
    static bool closeSecondaryWindow([[maybe_unused]] std::string_view page)
        requires(Derived::secondaryWindows)
    {
        if constexpr (Derived::compiled) {
            bool closed = false;
            try {
                invokeOnUIThread(uiExecutor(), [&] {
                    closed = Derived::closeSecondaryNative(page);
                });
            } catch (const UIExecutorUnavailable&) {
                return false;
            }
            return closed;
        } else {
            return false;
        }
    }

    static void open(std::string_view port, std::string_view file) {
        open("127.0.0.1", port, file);
    }
    template <typename D = Derived>  // dependent: the options type is only needed once the frontend is complete
    static void open(std::string_view address, std::string_view port, std::string_view file, const typename D::Options& options = startingOptions()) {
        if constexpr (Derived::compiled)
            Derived::openNative(address, port, file, options);
        else
            unavailable();
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy) {
        open(address, port, file, optionsOf(policy));
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
        open(address, port, file, optionsOf(policy, &window));
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window,
                     const ViewLaunch& launch) {
        open(address, port, file, optionsOf(policy, &window, &launch));
    }

private:
    template <typename D = Derived>
    static typename D::Options startingOptions() {
        if constexpr (requires { Derived::defaultOptions(); })
            return Derived::defaultOptions();
        else
            return typename Derived::Options{};
    }
    template <typename D = Derived>
    static typename D::Options optionsOf(const ContentPolicy& policy, const WindowOptions* window = nullptr, const ViewLaunch* launch = nullptr) {
        // The settings of the arguments the caller gave replace the configured ones; the others stay as configured.
        auto options          = startingOptions();
        options.contentPolicy = policy;
        if (window)
            options.window = *window;
        if (launch)
            options.launch = *launch;
        return options;
    }
    template <typename Function>
    static void onUIThread(Function&& function) {
        if constexpr (requires { requires Derived::waitsForFirstWindow; })
            invokeOnUIThreadWhenReady(uiExecutor(), function);
        else
            invokeOnUIThread(uiExecutor(), function);
    }
    [[noreturn]] static void unavailable() {
        throw std::runtime_error(std::string(Derived::name) + " is unavailable. " + std::string(Derived::enable));
    }
};

}  // namespace webfront::frontend
