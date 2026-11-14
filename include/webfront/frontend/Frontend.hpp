#pragma once

#include "../ContentPolicy.hpp"
#include "../ViewAssociation.hpp"
#include "Action.hpp"
#include "Window.hpp"
#include "CEF.hpp"
#include "CloseRequest.hpp"
#include "DefaultBrowser.hpp"
#include "NativeWebView.hpp"
#include "NativeWindow.hpp"
#include "UIExecutor.hpp"
#include "UILoop.hpp"
#include "WebView2.hpp"
#include "WKWebView.hpp"
#include "WebKitGTK.hpp"

#include <atomic>
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
 * An optional static void launch(address, port, file) opens the interface and returns without waiting for it: BasicWF::start()
 * needs it, and a frontend whose window lives in open()'s loop does not offer it. A launch(address, port, file, policy)
 * overload is preferred when present. A launched interface is an external one: it receives neither WindowOptions nor the
 * ViewLaunch proof (the system browser never does, ADR-020), so start() and show() treat such a frontend alike.
 * An optional static constexpr bool endsOnRequest = false tells that requestClose() cannot end every session
 * (frontend::endsOnRequest<Frontend>).
 * An optional static bool requestClose() asks, from any thread, the session that open() runs to end as if the user had
 * closed the window; it returns false when nothing is open to receive the request. BasicWF::requestClose() forwards it.
 * A frontend that publishes its interface thread (uiExecutor()) declares `requestClose(std::optional<UISession>)` instead:
 * BasicWF passes the session it owns, and the request is dropped on the interface thread if another loop runs by then.
 * An optional static constexpr bool requiresMainThread tells that open() belongs to the process main thread
 * (frontend::requiresMainThread<Frontend>); webfront::UIThread refuses such a frontend at compile time.
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
    static constexpr bool   requiresMainThread = false;

    static void initialize() { }

    /// Opens the page in the system browser and returns at once, without the console wait of open() and without waiting
    /// for the opener command to end (BasicWF::start()).
    static void launch(std::string_view address, std::string_view port, std::string_view file) {
        browser::launchWithoutWaiting(detail::applicationUrl(address, port, file));
    }

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
    // AppKit's run loop belongs to the main thread; the Windows message pump and the GLib loop run where open() is called.
#if defined(__APPLE__)
    static constexpr bool requiresMainThread = true;
#else
    static constexpr bool requiresMainThread = false;
#endif

    static void initialize() { }

    /// Ends the loop of a running open(), as the user closing the window would end the others; the browser's window is
    /// not ours to close, the loop is. False, and nothing remembered, when no loop runs: unlike requestStop(), a request
    /// never ends a later session.
    static bool requestClose([[maybe_unused]] std::optional<UISession> session = std::nullopt) noexcept {
        if (!session)
            return loopStop().requestIfRunning();
        // Checked under the stop's own lock, which the end of that loop takes too: the session cannot change in between.
        return loopStop().requestIfRunning([&] {
            return uiExecutor().session() == session;
        });
    }

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
        // The opener is not waited for: it can live as long as the browser, and requestClose() must find the loop meanwhile.
        loop(address, port, file, browser::launchWithoutWaiting);
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
    // CEF is initialized and run on the process main thread; another thread is not a qualified configuration.
    static constexpr bool requiresMainThread = true;

    /// Asks the window to close, from any thread; false when no window is open (see CloseRequest.hpp).
    static bool requestClose([[maybe_unused]] std::optional<UISession> session = std::nullopt) noexcept {
#ifdef WEBFRONT_EMBED_CEF
        return detail::requestWindowClose(uiExecutor(), nativeWindow(), [](const NativeWindowHandle& window) {
            cef::closeBrowser(window.pointer);
        }, session);
#else
        return false;
#endif
    }

    /// CEF's UI thread while open() runs its message loop (see UIExecutor.hpp).
    [[nodiscard]] static UIExecutor uiExecutor() {
        return uiExecutorHostOf<CEFFrontend>().executor();
    }

    /// The application's CefBrowser, readable on CEF's UI thread while open() runs (see NativeWindow.hpp).
    [[nodiscard]] static NativeWindow nativeWindow() {
        return nativeWindowHostOf<CEFFrontend>().window();
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
    static constexpr bool   requiresMainThread = frontend::requiresMainThread<Native> || frontend::requiresMainThread<Browser>;
    /// Whichever is used at run time must be closable on request: the plain system browser, which waits for Enter, is not.
    static constexpr bool   endsOnRequest = frontend::endsOnRequest<Native> && frontend::endsOnRequest<Browser>;

    /// Asks the one in use to end its session; false when it has nothing to close (the plain system browser never has).
    /// Callable from any thread: the decision of initialize() is read atomically.
    static bool requestClose(std::optional<UISession> session = std::nullopt) {
        if (usesNativeWindow())
            return closeWith<Native>(session);
        return closeWith<Browser>(session);
    }

    /// Why the browser is used, or nullopt when the native window is. Meaningful after initialize().
    [[nodiscard]] static std::optional<std::string> fallbackReason() {
        return state().reason;
    }
    [[nodiscard]] static bool usesNativeWindow() {
        return state().native.load();
    }
    /// Whether the server stops once open() returns: true for a native window, false for the browser.
    [[nodiscard]] static bool closeServerAfterOpen() {
        if constexpr (Native::action == Action::closeServerAfterOpen)
            return usesNativeWindow();
        else
            return false;
    }

    /// Whether the frontend in use publishes an interface thread at all (uiExecutor() is not just an empty handle): the
    /// owner of a session is told apart by it only then.
    [[nodiscard]] static bool publishesUIExecutor() {
        if (usesNativeWindow())
            return requires { { Native::uiExecutor() } -> std::same_as<UIExecutor>; };
        return requires { { Browser::uiExecutor() } -> std::same_as<UIExecutor>; };
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

    /// The window of the one in use; the system browser has none.
    [[nodiscard]] static NativeWindow nativeWindow() {
        if (usesNativeWindow()) {
            if constexpr (requires { { Native::nativeWindow() } -> std::same_as<NativeWindow>; })
                return Native::nativeWindow();
        }
        return {};
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
        s.native = !s.reason.has_value();
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
    template <typename Used>
    static bool closeWith(std::optional<UISession> session) {
        if constexpr (requires { { Used::requestClose(session) } -> std::same_as<bool>; })
            return Used::requestClose(session);
        else if constexpr (requires { { Used::requestClose() } -> std::same_as<bool>; })
            return Used::requestClose();
        else
            return false;
    }

    struct State {
        std::optional<std::string> reason;        // written by initialize() only
        std::atomic<bool>          native{true};  // the decision, readable from any thread (requestClose)
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
