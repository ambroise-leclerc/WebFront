#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <fstream>
#include <iostream>


namespace webfront::browser {
/// Check if running under WSL
bool isWSL() {
    std::ifstream proc_version("/proc/version");
    if (proc_version.is_open()) {
        std::string version_info;
        std::getline(proc_version, version_info);
        return version_info.find("Microsoft") != std::string::npos || 
               version_info.find("WSL") != std::string::npos;
    }
    return false;
}

/// Open the web UI in the system's default browser (blocking until user closes)
void open(std::string_view port, std::string_view file) {
    std::string command;
    
#ifdef _WIN32
    command = "start ";
#elif __linux__
    // Check if we're running under WSL
    if (isWSL()) {
        command = "/mnt/c/Windows/System32/cmd.exe /c start ";
    } else {
        command = "xdg-open ";
    }
#elif __APPLE__
    command = "open ";
#endif

    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    auto systemResult = ::system(command.append(url).c_str());
    if (systemResult != 0) {
        throw std::runtime_error("Failed to open browser with command: " + command + url);
    }
    
    // Make this function blocking like CEF - wait for user to close browser
    std::cout << "Browser opened. Press Enter to close and stop the server..." << std::endl;
    std::cin.get();
}

} // namespace webfront