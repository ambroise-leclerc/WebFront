#pragma once
#include <webfront/WebFront.hpp>

#include <cstdlib>
#include <memory>

namespace webfront::testdriver {
inline bool diagnosticWindows() {
#ifdef _MSC_VER
    char*       raw  = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "WEBFRONT_TEST_VISIBLE") != 0)
        throw std::runtime_error("Cannot read WEBFRONT_TEST_VISIBLE");
    const std::unique_ptr<char, decltype(&std::free)> value(raw, std::free);
    return value && std::string_view(value.get()) == "1";
#else
    const auto* value = std::getenv("WEBFRONT_TEST_VISIBLE");
    return value && std::string_view(value) == "1";
#endif
}

template <typename Frontend>
struct QuietFrontend : Frontend {
    using Frontend::open;
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, ContentPolicy{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy = {}) {
        if constexpr (std::is_same_v<Frontend, frontend::WKWebViewFrontend>) {
            frontend::WKWebViewOptions options;
            options.contentPolicy = policy;
            options.visible       = diagnosticWindows();
            Frontend::open(address, port, page, options);
        } else if constexpr (std::is_same_v<Frontend, frontend::WebView2Frontend>) {
            frontend::WebView2Options options;
            options.contentPolicy = policy;
            options.visible       = diagnosticWindows();
            Frontend::open(address, port, page, options);
        } else {
            // Linux engines use one Xvfb display around a complete suite; their native windows stay real.
            Frontend::open(address, port, page, policy);
        }
    }
};
}  // namespace webfront::testdriver
