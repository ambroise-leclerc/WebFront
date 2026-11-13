// HTTP diagnostics for native test processes. Keep bridge traffic out of the transcript so CTest's output cap
// does not discard the resource requests we need when a session frame fails to load (#542).
#pragma once

#include <webfront/tooling/Logger.hpp>

#include <array>
#include <iostream>
#include <mutex>
#include <string_view>

namespace webfront::testdriver {
// Markers of the lines the HTTP layer logs for a request (HTTPServer.hpp). Rename a message there and update this
// list: LoggerTests checks the filter against the server's current wording.
inline constexpr std::array<std::string_view, 5> requestMarkers{
    "Received request ", "responding http ", "HTTP asset request ", "ended its response early", "Route "};

inline bool isRequestLine(std::string_view line) {
    // Warnings and errors are always kept; Info lines only when they describe a request.
    if (line.find("[I]") == std::string_view::npos)
        return true;
    for (const auto marker : requestMarkers)
        if (line.find(marker) != std::string_view::npos)
            return true;
    return false;
}

inline auto logRequests() {
    log::setLogLevel(log::Info);
    return log::addSinks([](std::string_view line) {
        if (isRequestLine(line)) {
            static std::mutex      output;
            const std::scoped_lock lock{output};
            std::clog << line << '\n';
        }
    });
}
}  // namespace webfront::testdriver
