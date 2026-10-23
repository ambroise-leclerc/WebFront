/// @brief Relative paths received from the browser, checked to stay inside the directory they name a file of.
#pragma once

#include <cstddef>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace webfront::fs {

namespace detail {

inline std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// The HTTP server rejects every URL containing "..", so a path containing it could be written but never served.
inline bool hasSafeCharacters(std::string_view path) {
    using namespace std::string_view_literals;
    if (path.size() > 4096 || path.find("..") != std::string_view::npos)
        return false;
    return path.find_first_of("\\:*?\"<>|\0"sv) == std::string_view::npos;
}

// Appends a component unless it is empty, "." or ".." (or "...", which no path needs).
inline bool appendComponent(std::filesystem::path& path, std::string_view component) {
    if (component.find_first_not_of('.') == std::string_view::npos)
        return false;
    path /= fromUtf8(component);
    return true;
}

}  // namespace detail

/// Converts a '/'-separated path received from the browser into a relative path that stays inside
/// its directory. Rejects empty and absolute paths, empty, "." and ".." components, backslashes,
/// colons (drive letters and alternate streams), the other characters Windows forbids in file names
/// (*?"<>|) and NUL, on every platform, with std::invalid_argument.
inline std::filesystem::path safeRelativePath(std::string_view path) {
    std::filesystem::path result;
    bool                  safe = detail::hasSafeCharacters(path);
    for (const auto part : std::views::split(path, '/'))
        safe = safe && detail::appendComponent(result, std::string_view(part.begin(), part.end()));
    if (!safe || result.empty())
        throw std::invalid_argument("Unsafe path '" + std::string(path) + "'");
    return result;
}

}  // namespace webfront::fs
