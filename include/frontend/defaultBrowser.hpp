#pragma once

#include <string_view>

namespace webfront {
namespace frontend {

/// Open the web UI in the system's default browser
auto openInDefaultBrowser(std::string_view port, std::string_view file) {
#ifdef _WIN32
    auto command = std::string("start ");
#elif __linux__
    auto command = std::string("xdg-open ");
#elif __APPLE__
    auto command = std::string("open ");
#endif

    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    return ::system(command.append(url).c_str());
}

} // namespace frontend
} // namespace webfront
