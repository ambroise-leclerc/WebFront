#pragma once

#include <webfront/frontend/Capture.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace boing {

inline void validateCapture(bool requested, bool nativeAvailable, bool browser, bool benchmark) {
    if (requested && (!nativeAvailable || browser || benchmark))
        throw std::invalid_argument("--capture requires a native window and cannot be combined with --browser or --benchmark");
}

/// Bound sequence capture to 600 frames, with a fixed 25 fps scene clock.
inline std::optional<unsigned> captureFrameCount(std::string_view text) {
    unsigned count{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc{} || end != text.data() + text.size() || count == 0 || count > 600)
        return std::nullopt;
    return count;
}

inline std::string captureFramePath(const std::string& path, unsigned index, unsigned count) {
    if (count == 1)
        return path;
    const std::filesystem::path base{path};
    const auto                  number = std::to_string(index);
    return (base.parent_path() / (base.stem().string() + "-" + std::string(4 - number.size(), '0') + number + ".png")).string();
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
