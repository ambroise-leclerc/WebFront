#pragma once
#include "../system/WindowsCompat.hpp"
#include "Url.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
    return "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive -Command "
           + shellArgument("Start-Process -FilePath '" + url + "'");
}

inline std::string commandForUrl(const std::string& url) {
#ifdef _WIN32
    return "start \"\" \"" + url + "\"";
#elif __linux__
    if (isWSL())
        return wslCommandForUrl(url);
    return "xdg-open '" + url + "'";
#elif __APPLE__
    return "open '" + url + "'";
#else
    return "'" + url + "'";
#endif
}

inline std::string command(std::string_view port, std::string_view file) {
    return commandForUrl(frontend::detail::applicationUrl(port, file));
}

// An actual desktop launch cannot run in unattended coverage. Session behaviour
// below is exercised with an injected launcher and streams.
// LCOV_EXCL_START
inline void launch(const std::string& url) {
#ifdef _WIN32
    const auto result = reinterpret_cast<std::intptr_t>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        throw std::runtime_error("Failed to open system browser (ShellExecute error " + std::to_string(result) + ")");
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
