#pragma once

#include "CEF.hpp"
#include "DefaultBrowser.hpp"

#include <concepts>
#include <cstdlib>
#include <string_view>
#include <type_traits>

namespace webfront::frontend {

enum class Action {
    keepServerRunning,
    closeServerAfterOpen
};

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
