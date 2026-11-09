#pragma once

#include "../ContentPolicy.hpp"
#include "../ViewAssociation.hpp"
#include "Action.hpp"
#include "Window.hpp"
#include "CEF.hpp"
#include "DefaultBrowser.hpp"
#include "NativeWebView.hpp"
#include "UIExecutor.hpp"
#include "UILoop.hpp"
#include "WebView2.hpp"
#include "WKWebView.hpp"
#include "WebKitGTK.hpp"

#include <concepts>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace webfront::frontend {

/**
 * @brief Frontend contract; behavioural details and trusted-content scope are in docs/FRONTENDS.md.
 *
 * initialize() runs once successfully per frontend type, during the first BasicWF construction,
 * before the listener is created. If it throws, construction fails and a later construction retries.
 * Construct the application and call show()/openAndRun() on the main thread for embedded frontends;
 * the adapter enforces its toolkit's thread requirements (WebView2 requires STA-compatible COM).
 *
 * open(port, file) runs on the calling thread while the server runs on a separate thread.
 * With closeServerAfterOpen it blocks until window closure and releases its session resources;
 * show() then stops and joins the server. With keepServerRunning, returning leaves the server active
 * until stop()/destruction; this action does not require open() itself to return immediately.
 * If open() throws, show()/openAndRun() stop and join the server before rethrowing the original
 * exception, regardless of action. Direct openWindow() calls only delegate and return the action.
 *
 * An unavailable frontend remains declared and throws during initialization or opening with a
 * diagnostic naming its enabling CMake option. ContentPolicy rules and adapter capabilities
 * are documented in docs/CONTENT_POLICY.md.
 * An optional open(address, port, file, policy, window) overload also receives the application's WindowOptions.
 * An optional static UIExecutor uiExecutor() publishes the interface thread (UIExecutor.hpp): it is available while
 * open() runs the frontend's loop, and BasicWF::uiExecutor() forwards it.
 * An optional static bool closeServerAfterOpen() overrides `action` at run time (NativeOrBrowserFrontend).
 * An optional open(address, port, file) overload receives the configured address; build URLs with
 * detail::applicationUrl() so all frontends preserve the same URI-reference semantics.
 */
template <typename Frontend>
concept FrontendType = requires(std::string_view port, std::string_view file) {
    { Frontend::initialize() } -> std::same_as<void>;
    { Frontend::open(port, file) } -> std::same_as<void>;
} && std::same_as<std::remove_cvref_t<decltype(Frontend::action)>, Action>;

// `action` describes what BasicWF should do after `open()` returns.
// Keep the server running when `open()` only launches the UI, and ask BasicWF to
// stop it when `open()` returns after the UI session is finished.
struct DefaultBrowserFrontend {
    static constexpr Action action{Action::keepServerRunning};

    static void initialize() { }

    static void open(std::string_view port, std::string_view file) {
        browser::open(port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file) {
        browser::open(address, port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy) {
        (void)policy;
        // External browsers own their navigation/UI; bridge authorization remains server-side.
        browser::open(address, port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
        (void)window;  // The system browser owns its window.
        open(address, port, file, policy);
    }
};

/**
 * @brief The system browser with an interface-thread loop on the calling thread (ADR-020, #462).
 *
 * Like DefaultBrowserFrontend it launches the page in the system browser, then runs the platform's minimal loop (Windows
 * message pump, AppKit run loop in the Accessory policy, GLib main loop) on the thread that called show()/open(), usually
 * the main thread. `uiExecutor()` is available while it runs, which the native dialogs need. Unlike the plain browser
 * frontend it does not wait for Enter: it returns when `requestStop()` is called, from any thread (a callback, a signal
 * handler's thread, a test). The server keeps running after it returns, until stop() or destruction. Without this frontend
 * the system browser has no interface thread and no executor.
 */
struct BrowserWithUILoopFrontend {
    static constexpr Action action{Action::keepServerRunning};

    static void initialize() { }

    /// Ends the loop of a running open(), or the next one at its start; callable from any thread.
    static void requestStop() noexcept {
        loopStop().request();
    }
    [[nodiscard]] static UIExecutor uiExecutor() {
        return uiExecutorHostOf<BrowserWithUILoopFrontend>().executor();
    }

    static void open(std::string_view port, std::string_view file) {
        open("127.0.0.1", port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file) {
        loop(address, port, file, browser::launch);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy&) {
        open(address, port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy&, const WindowOptions&) {
        open(address, port, file);
    }
    /// The loop with another launcher: the unit tests start no browser.
    static void loop(std::string_view address, std::string_view port, std::string_view file, browser::Launcher launcher) {
        // A launcher that fails throws before the loop: no executor is left attached.
        launcher(detail::applicationUrl(address, port, file));
        detail::uiloop::run(uiExecutorHostOf<BrowserWithUILoopFrontend>(), loopStop());
    }

private:
    static UILoopStop& loopStop() {
        static UILoopStop stop;
        return stop;
    }
};

struct CEFFrontend {
    static constexpr Action action{Action::closeServerAfterOpen};

    /// CEF's UI thread while open() runs its message loop (see UIExecutor.hpp).
    [[nodiscard]] static UIExecutor uiExecutor() {
        return uiExecutorHostOf<CEFFrontend>().executor();
    }

    static void initialize() {
        try {
            cef::initialize();
        } catch (const cef::CEFSubprocessExit& e) {
            std::exit(e.exit_code());
        }
    }

    static void open(std::string_view port, std::string_view file) {
        cef::open(port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file) {
        cef::open(address, port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy) {
        cef::open(address, port, file, policy);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
        cef::open(address, port, file, policy, window);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window,
                     const ViewLaunch& launch) {
        cef::open(address, port, file, policy, window, launch);
    }
};

namespace detail {

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
using PlatformNativeFrontend = NativeWebViewFrontend;
#else
// No native engine exists on this platform: the fallback always takes the browser.
struct PlatformNativeFrontend {
    static constexpr Action action{Action::closeServerAfterOpen};

    static void initialize() {
        throw std::runtime_error("no native web view exists for this platform");
    }
    static void open(std::string_view, std::string_view) {
        initialize();
    }
};
#endif

// Calls the richest open() the frontend offers, as BasicWF does.
template <typename Frontend>
void openWith(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window,
              const ViewLaunch& launch = {}) {
    if constexpr (requires { Frontend::open(address, port, file, policy, window, launch); })
        Frontend::open(address, port, file, policy, window, launch);
    else if constexpr (requires { Frontend::open(address, port, file, policy, window); })
        Frontend::open(address, port, file, policy, window);
    else if constexpr (requires { Frontend::open(address, port, file, policy); })
        Frontend::open(address, port, file, policy);
    else if constexpr (requires { Frontend::open(address, port, file); })
        Frontend::open(address, port, file);
    else
        Frontend::open(port, file);
}

}  // namespace detail

/**
 * @brief A native window when one can be opened, the system browser otherwise.
 *
 * initialize() tries Native::initialize(); if it throws (engine not compiled in, no display, missing runtime), the
 * frontend remembers the reason, prints it once to std::cerr and uses Browser for the whole process. The decision is
 * per frontend type and made once, like every initialize(). Only initialize() triggers the fallback: an exception
 * from the native open() propagates, because a window that failed after it started cannot be told apart from one
 * the user closed. Applications need neither a WEBFRONT_EMBED_* macro nor a
 * second application type.
 *
 * `action` is that of the native window; the browser path is reported at run time through closeServerAfterOpen(),
 * which the application consults after open() returns.
 */
template <FrontendType Native, FrontendType Browser>
struct NativeOrBrowserFrontendFor {
    static constexpr Action action{Native::action};

    /// Why the browser is used, or nullopt when the native window is. Meaningful after initialize().
    [[nodiscard]] static std::optional<std::string> fallbackReason() {
        return state().reason;
    }
    [[nodiscard]] static bool usesNativeWindow() {
        return !state().reason.has_value();
    }
    /// Whether the server stops once open() returns: true for a native window, false for the browser.
    [[nodiscard]] static bool closeServerAfterOpen() {
        if constexpr (Native::action == Action::closeServerAfterOpen)
            return usesNativeWindow();
        else
            return false;
    }

    /// The interface thread of the window in use; the browser path has one only with its loop mode.
    [[nodiscard]] static UIExecutor uiExecutor() {
        if (usesNativeWindow()) {
            if constexpr (requires { { Native::uiExecutor() } -> std::same_as<UIExecutor>; })
                return Native::uiExecutor();
            else
                return {};
        } else {
            if constexpr (requires { { Browser::uiExecutor() } -> std::same_as<UIExecutor>; })
                return Browser::uiExecutor();
            else
                return {};
        }
    }

    // Called once per process by BasicWF (std::call_once). It resets the decision, so a direct caller must not
    // race it with open(); the unit tests call it repeatedly on purpose.
    static void initialize() {
        auto& s = state();
        s.reason.reset();
        try {
            Native::initialize();
        } catch (const std::exception& e) {
            s.reason = e.what();
        }
        if (s.reason) {
            std::cerr << "WebFront: opening the system browser instead of a native window: " << *s.reason << '\n';
            Browser::initialize();
        }
    }

    static void open(std::string_view port, std::string_view file) {
        if (usesNativeWindow())
            Native::open(port, file);
        else
            Browser::open(port, file);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file) {
        open(address, port, file, ContentPolicy{}, WindowOptions{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy) {
        open(address, port, file, policy, WindowOptions{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
        open(address, port, file, policy, window, ViewLaunch{});
    }
    /// The launch proof goes to the native window only: the system browser is never given a secret (ADR-020).
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window,
                     const ViewLaunch& launch) {
        if (usesNativeWindow())
            detail::openWith<Native>(address, port, file, policy, window, launch);
        else
            detail::openWith<Browser>(address, port, file, policy, window);
    }

private:
    struct State {
        std::optional<std::string> reason;
    };
    static State& state() {
        static State instance;
        return instance;
    }
};

using NativeOrBrowserFrontend = NativeOrBrowserFrontendFor<detail::PlatformNativeFrontend, DefaultBrowserFrontend>;

// WEBFRONT_EMBED_CEF only means "CEF is available"; it never changes which frontend
// plain WebFront selects. Applications opt into CEF explicitly via WebFrontWithFrontend<CEFFrontend>.
using DefaultFrontend = DefaultBrowserFrontend;

}  // namespace webfront::frontend
