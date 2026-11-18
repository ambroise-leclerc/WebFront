#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
    #include "../system/WindowsCompat.hpp"
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

namespace webfront {

/**
 * @brief Initial native window of the embedded frontends (the system browser ignores it).
 *
 * An empty title selects the executable's name. Sizes are in the toolkit's logical pixels; a
 * non-positive size selects the default. `background` is a CSS hexadecimal colour ("#rgb", "#rrggbb" or "#rrggbbaa") painted
 * under the page, so that the window is never black or white against a contrary theme before the first frame; empty keeps the
 * toolkit's default and an invalid value is ignored. `media` is described below.
 */
struct WindowOptions {
    std::string title{};
    int         width{1200};
    int         height{800};
    std::string background{};
    /// Whether the page may play audio or video. WebKitGTK initializes GStreamer, and scans its plugins, in the page's process
    /// as soon as a page loads, even if it never plays anything (up to 1.7 s on a cold plugin registry). False, the default,
    /// gives that process no plugins, so nothing is initialized and a page's `<audio>` and `<video>` cannot play. Set it
    /// to true for an application with media. Frontends other than WebKitGTK ignore it.
    bool media{false};
};

namespace frontend::detail {

inline std::string executableName() {
    std::filesystem::path path;
    try {
    #if defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        buffer.resize(GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())));
        path = buffer;
    #elif defined(__APPLE__)
        std::string   buffer(4096, '\0');
        std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
        if (_NSGetExecutablePath(buffer.data(), &size) == 0)
            path = buffer.c_str();
    #elif defined(__linux__)
        path = std::filesystem::read_symlink("/proc/self/exe");
    #endif
        const auto name = path.stem().u8string();
        return {name.begin(), name.end()};
    } catch (...) {
        return {};
    }
}

/// The components (0 to 1) of a "#rgb", "#rrggbb" or "#rrggbbaa" colour; nothing for any other text.
inline std::optional<std::array<float, 4>> parseBackground(std::string_view text) {
    if (text.size() < 2 || text.front() != '#')
        return std::nullopt;
    text.remove_prefix(1);
    if (text.size() != 3 && text.size() != 6 && text.size() != 8)
        return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::array<float, 4> rgba{0, 0, 0, 1};
    const std::size_t    width = text.size() == 3 ? 1 : 2;
    for (std::size_t i = 0; i * width < text.size(); ++i) {
        const int high = digit(text[i * width]);
        const int low  = width == 1 ? high : digit(text[i * width + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        rgba[i] = static_cast<float>(high * 16 + low) / 255.0f;
    }
    return rgba;
}

/// The window's effective values: title from the application, else the executable's name, else "WebFront".
inline WindowOptions resolveWindow(WindowOptions window) {
    if (window.title.empty())
        window.title = executableName();
    if (window.title.empty())
        window.title = "WebFront";
    const WindowOptions defaults;
    if (window.width <= 0)
        window.width = defaults.width;
    if (window.height <= 0)
        window.height = defaults.height;
    return window;
}

}  // namespace frontend::detail
}  // namespace webfront
