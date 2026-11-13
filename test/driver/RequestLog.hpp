// HTTP diagnostics for native test processes. Keep bridge traffic out of the transcript so CTest's output cap
// does not discard the resource requests we need when a session frame fails to load (#542).
#pragma once

#include <webfront/tooling/Logger.hpp>

#include <iostream>
#include <mutex>
#include <string_view>

namespace webfront::testdriver {
inline auto logRequests() {
    log::setLogLevel(log::Info);
    return log::addSinks([](std::string_view line) {
        if (line.find("[I]") == std::string_view::npos || line.find("Received request ") != std::string_view::npos
            || line.find("responding http ") != std::string_view::npos || line.find("HTTP asset request ") != std::string_view::npos
            || line.find("ended its response early") != std::string_view::npos || line.find("Route ") != std::string_view::npos) {
            static std::mutex      output;
            const std::scoped_lock lock{output};
            std::clog << line << '\n';
        }
    });
}
}  // namespace webfront::testdriver
