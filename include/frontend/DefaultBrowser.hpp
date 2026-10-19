#pragma once
#include "../system/WindowsCompat.hpp"
#include "Url.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
    #include <shellapi.h>
#endif

namespace webfront::browser {

inline bool isWSL() {
    std::ifstream proc_version("/proc/version");
    if (proc_version.is_open()) {
        std::string version_info;
        std::getline(proc_version, version_info);
        return version_info.find("Microsoft") != std::string::npos || version_info.find("WSL") != std::string::npos;
    }
    return false;
}

inline std::string shellArgument(std::string_view value) {
    std::string quoted = "'";
    for (const auto character : value) {
        if (character == '\'')
            quoted += "'\\''";
        else
            quoted += character;
    }
    return quoted + "'";
}

inline std::string wslCommandForUrl(const std::string& url) {
    // PowerShell single quotes preserve %, $, and &; POSIX quoting preserves the script.
    std::string escaped;
    for (const auto character : url) {
        escaped += character;
        if (character == '\'')
            escaped += '\'';
    }
    return "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive -Command "
           + shellArgument("Start-Process -FilePath '" + escaped + "'");
}

inline std::string commandForUrl(const std::string& url) {
#ifdef _WIN32
    return "start \"\" \"" + url + "\"";
#elif __linux__
    if (isWSL())
        return wslCommandForUrl(url);
    return "xdg-open " + shellArgument(url);
#elif __APPLE__
    return "open " + shellArgument(url);
#else
    return "'" + url + "'";
#endif
}

inline std::string command(std::string_view port, std::string_view file) {
    return commandForUrl(frontend::detail::applicationUrl(port, file));
}

#ifdef _WIN32
namespace detail {
inline std::wstring wideBrowserUrl(std::string_view url) {
    if (url.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("System browser URL is too long");
    const auto length = static_cast<int>(url.size());
    const int  size   = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), length, nullptr, 0);
    if (!size)
        throw std::runtime_error("System browser URL is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), length, result.data(), size))
        throw std::runtime_error("System browser URL conversion failed");
    return result;
}

template <typename ShellExecutor>
void launchWindowsUrl(std::string_view url, ShellExecutor execute) {
    const auto wideUrl = wideBrowserUrl(url);
    const auto result  = reinterpret_cast<std::intptr_t>(execute(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        throw std::runtime_error("Failed to open system browser (ShellExecute error " + std::to_string(result) + ")");
}
}  // namespace detail
#endif

// An actual desktop launch cannot run in unattended coverage. Session behaviour
// below is exercised with an injected launcher and streams.
// LCOV_EXCL_START
inline void launch(const std::string& url) {
#ifdef _WIN32
    detail::launchWindowsUrl(url, ShellExecuteW);
#else
    const auto invocation = commandForUrl(url);
    if (::system(invocation.c_str()) != 0)
        throw std::runtime_error("Failed to open browser with command: " + invocation);
#endif
}
// LCOV_EXCL_STOP

using Launcher = void (*)(const std::string&);

struct LaunchContext {
    Launcher      launcher = launch;
    std::istream& input    = std::cin;
    std::ostream& output   = std::cout;
};

/// Launch the page, then wait for Enter before returning control to the caller.
inline void open(std::string_view address, std::string_view port, std::string_view file, const LaunchContext& context = {}) {
    context.launcher(frontend::detail::applicationUrl(address, port, file));
    context.output << "Browser opened. Press Enter to close and stop the server..." << std::endl;
    context.input.get();
}

inline void open(std::string_view port, std::string_view file, const LaunchContext& context = {}) {
    open("127.0.0.1", port, file, context);
}

}  // namespace webfront::browser
