#pragma once
#include "Url.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <fstream>
#include <iostream>


namespace webfront::browser {
/// Check if running under WSL
inline bool isWSL() {
    std::ifstream proc_version("/proc/version");
    if (proc_version.is_open()) {
        std::string version_info;
        std::getline(proc_version, version_info);
        return version_info.find("Microsoft") != std::string::npos || 
               version_info.find("WSL") != std::string::npos;
    }
    return false;
}

/// Shell command that opens the application page in the system browser. The URL comes from the shared
/// builder, so the page path is percent-encoded and contains no shell metacharacter; it is still quoted,
/// and the port, which is not encoded, must consist of digits only.
inline std::string command(std::string_view port, std::string_view file) {
    if (port.empty() || port.find_first_not_of("0123456789") != std::string_view::npos)
        throw std::invalid_argument("The port must be a decimal number: " + std::string(port));
    const auto url = frontend::detail::applicationUrl(port, file);
#ifdef _WIN32
    // The first quoted argument of `start` is the window title.
    return "start \"\" \"" + url + "\"";
#elif __linux__
    // Check if we're running under WSL
    if (isWSL())
        return "/mnt/c/Windows/System32/cmd.exe /c start \"\" \"" + url + "\"";
    return "xdg-open \"" + url + "\"";
#elif __APPLE__
    return "open \"" + url + "\"";
#else
    return "\"" + url + "\"";
#endif
}

/// Open the web UI in the system's default browser (blocking until user closes)
inline void open(std::string_view port, std::string_view file) {
    const auto launch = command(port, file);
    if (::system(launch.c_str()) != 0)
        throw std::runtime_error("Failed to open browser with command: " + launch);

    // Make this function blocking like CEF - wait for user to close browser
    std::cout << "Browser opened. Press Enter to close and stop the server..." << std::endl;
    std::cin.get();
}

} // namespace webfront