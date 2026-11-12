#pragma once
#include <webfront/WebFront.hpp>

#include <cstdlib>
#include <memory>
#include <string_view>
#include <type_traits>

namespace webfront::testdriver {
// Rendering stays visible in daily runs; repeated openings are reduced by sharing sessions, not by hiding windows.
inline constexpr bool visibleWindows = true;

template <typename Frontend>
struct TestFrontend : Frontend {
    using Frontend::open;
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, ContentPolicy{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy = {}) {
        open(address, port, page, policy, WindowOptions{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy, const WindowOptions& window) {
        open(address, port, page, policy, window, ViewLaunch{});
    }
    // The launch proof reaches the engine as it does in an application: the suite runs the same association.
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy, const WindowOptions& window,
                     const ViewLaunch& launch) {
        if constexpr (std::is_same_v<Frontend, frontend::WKWebViewFrontend>) {
            frontend::WKWebViewOptions options;
            options.contentPolicy = policy;
            options.window        = window;
            options.launch        = launch;
            options.visible       = visibleWindows;
            Frontend::open(address, port, page, options);
        } else if constexpr (std::is_same_v<Frontend, frontend::WebView2Frontend>) {
            frontend::WebView2Options options;
            options.contentPolicy = policy;
            options.window        = window;
            options.visible       = visibleWindows;
            options.launch        = launch;
            Frontend::open(address, port, page, options);
        } else {
            // Linux engines use one Xvfb display around a complete suite; their native windows stay real.
            Frontend::open(address, port, page, policy, window, launch);
        }
    }
};
}  // namespace webfront::testdriver
