/// @brief Relative paths received from the browser, checked to stay inside the directory they name a file of.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

inline char lowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The device names Windows reserves in every directory, whatever their extension and case: "con", "nul.txt" and
// "COM1.tar.gz" all name a device. The superscript digits ¹²³ also complete COM and LPT.
inline bool isReservedWindowsName(std::string_view component) {
    auto stem = component.substr(0, component.find('.'));
    while (stem.ends_with(' '))
        stem.remove_suffix(1);
    std::string lower;
    std::ranges::transform(stem, std::back_inserter(lower), lowerAscii);
    static constexpr std::array<std::string_view, 6> devices{"con", "prn", "aux", "nul", "conin$", "conout$"};
    if (std::ranges::find(devices, lower) != devices.end())
        return true;
    if (!lower.starts_with("com") && !lower.starts_with("lpt"))
        return false;
    const auto suffix = std::string_view(lower).substr(3);
    return (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') || suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3";
}

// A file name every supported file system stores as written: not empty, not made of dots only ("." and ".." are
// links), no trailing dot or space (Windows drops them, so "a." would name "a"), no reserved Windows device name.
inline bool isSafeName(std::string_view component) {
    if (component.find_first_not_of('.') == std::string_view::npos)
        return false;
    if (component.ends_with('.') || component.ends_with(' '))
        return false;
    return !isReservedWindowsName(component);
}

inline bool appendComponent(std::filesystem::path& path, std::string_view component) {
    if (!isSafeName(component))
        return false;
    path /= fromUtf8(component);
    return true;
}

}  // namespace detail

/// True for a single file name that safeRelativePath() would accept as a component: a name a storage can create on
/// every platform and that never names a parent, a device or another file.
inline bool isSafeFileName(std::string_view name) {
    return !name.empty() && detail::hasSafeCharacters(name) && name.find('/') == std::string_view::npos && detail::isSafeName(name);
}

/// Converts a '/'-separated path received from the browser into a relative path that stays inside
/// its directory. Rejects empty and absolute paths, empty, "." and ".." components, backslashes,
/// colons (drive letters and alternate streams), the other characters Windows forbids in file names
/// (*?"<>|) and NUL, components ending with a dot or a space and the device
/// names Windows reserves (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to LPT9, with any extension), on every
/// platform, with std::invalid_argument.
///
/// This checks the syntax only. A path that passes may still leave its directory on disk through a symbolic link
/// or a junction: confinement is enforced while resolving it, relative to an opened directory (fs::NativeStorage).
inline std::filesystem::path safeRelativePath(std::string_view path) {
    std::filesystem::path result;
    bool                  safe = detail::hasSafeCharacters(path);
    for (const auto part : std::views::split(path, '/'))
        safe = safe && detail::appendComponent(result, std::string_view(part.begin(), part.end()));
    if (!safe || result.empty())
        throw std::invalid_argument("Unsafe path '" + std::string(path) + "'");
    return result;
}

/// The components of a '/'-separated path inside a storage root, checked as safeRelativePath() checks them. The empty
/// path names the root itself and yields no component.
/// @throws std::invalid_argument for a path safeRelativePath() would reject
inline std::vector<std::string> safePathComponents(std::string_view path) {
    std::vector<std::string> components;
    if (path.empty())
        return components;
    bool safe = detail::hasSafeCharacters(path);
    for (const auto part : std::views::split(path, '/')) {
        const std::string_view component(part.begin(), part.end());
        safe = safe && detail::isSafeName(component);
        if (!safe)
            break;
        components.emplace_back(component);
    }
    if (!safe)
        throw std::invalid_argument("Unsafe path '" + std::string(path) + "'");
    return components;
}

}  // namespace webfront::fs
