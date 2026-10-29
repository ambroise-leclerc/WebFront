#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

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
 * non-positive size selects the default.
 */
struct WindowOptions {
    std::string title{};
    int         width{1200};
    int         height{800};
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
