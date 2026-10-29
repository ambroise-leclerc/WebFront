#pragma once

#include "../ContentPolicy.hpp"
#include "Action.hpp"
#include "Window.hpp"
#include "CEF.hpp"
#include "DefaultBrowser.hpp"
#include "NativeWebView.hpp"
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

struct CEFFrontend {
    static constexpr Action action{Action::closeServerAfterOpen};

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
void openWith(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
    if constexpr (requires { Frontend::open(address, port, file, policy, window); })
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
        if (usesNativeWindow())
            detail::openWith<Native>(address, port, file, policy, window);
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
