#pragma once

#include <webfront/frontend/Capture.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace boing {

inline void validateCapture(bool requested, bool nativeAvailable, bool browser, bool benchmark) {
    if (requested && (!nativeAvailable || browser || benchmark))
        throw std::invalid_argument("--capture requires a native window and cannot be combined with --browser or --benchmark");
}

/// Includes close errors: a successful buffered write alone does not establish that the file was saved.
inline void saveCapture(const webfront::frontend::CaptureResult& png, const std::string& path) {
    if (!png)
        throw std::runtime_error("No capture: " + png.error());
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()));
    file.close();
    if (!file)
        throw std::runtime_error("No capture: cannot write " + path);
}

}  // namespace boing
