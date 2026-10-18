#pragma once

#include "Action.hpp"
#include "CEF.hpp"
#include "DefaultBrowser.hpp"
#include "WebView2.hpp"

#include <concepts>
#include <cstdlib>
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
 * diagnostic naming its enabling CMake option. Built-in frontends target trusted application content;
 * they supply no shared navigation, popup or bridge-origin authorization policy.
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
};

// WEBFRONT_EMBED_CEF only means "CEF is available"; it never changes which frontend
// plain WebFront selects. Applications opt into CEF explicitly via WebFrontWithFrontend<CEFFrontend>.
using DefaultFrontend = DefaultBrowserFrontend;

}  // namespace webfront::frontend
