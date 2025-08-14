#pragma once
#include <stdexcept>
#include <string>
#include <string_view>


namespace webfront::browser {
/// Open the web UI in the system's default browser
void open(std::string_view port, std::string_view file) {
#ifdef _WIN32
    auto command = std::string("start ");
#elif __linux__
    auto command = std::string("xdg-open ");
#elif __APPLE__
    auto command = std::string("open ");
#endif

    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    auto systemResult = ::system(command.append(url).c_str());
    if (systemResult != 0) {
        throw std::runtime_error("Failed to open browser with command: " + command + url);
    }
}

} // namespace webfront