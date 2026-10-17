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

// Frontend contract (details in docs/FRONTENDS.md). A frontend provides:
//   static void initialize()                              called once per frontend type, when the first BasicWF is constructed;
//   static void open(string_view port, string_view file)  called by show()/openAndRun() on the calling thread while the server runs
//                                                         on its own; build the page URL with detail::applicationUrl();
//   static constexpr Action action                        what BasicWF does when open() returns.
// An embedded window must be opened from the application's main thread: toolkits such as AppKit and GTK
// require it, and WebView2 needs an STA thread. open() blocks until the window closes, releases everything
// it created, and reports failure by throwing; BasicWF then stops and joins the server and rethrows,
// whatever the action. An unavailable frontend is still declared and throws from open() with a message
// naming the CMake option that enables it.
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

    static void initialize() {}

    static void open(std::string_view port, std::string_view file) {
        browser::open(port, file);
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
};

// WEBFRONT_EMBED_CEF only means "CEF is available"; it never changes which frontend
// plain WebFront selects. Applications opt into CEF explicitly via WebFrontWithFrontend<CEFFrontend>.
using DefaultFrontend = DefaultBrowserFrontend;

}  // namespace webfront::frontend
