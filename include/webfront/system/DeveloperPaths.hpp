/// @brief Path containment helpers of the developer store.
#pragma once

#include "SafePath.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace webfront::developer {

/// Paths from this length on take the extended-length form on Windows, below MAX_PATH (260) to leave room for appended names.
inline constexpr std::size_t extendedPathThreshold = 200;

#ifdef _WIN32
// Spelled with \x5c (a backslash): the prefixes are \?\ and \?\UNC\ for a network share, which is \ in its ordinary form.
inline constexpr std::wstring_view networkPrefix  = L"\x5c" L"\x5c";
inline constexpr std::wstring_view extendedPrefix = L"\x5c" L"\x5c" L"?" L"\x5c";
inline constexpr std::wstring_view uncPrefix      = L"\x5c" L"\x5c" L"?" L"\x5c" L"UNC" L"\x5c";
#endif

/// The form of an absolute path that the Windows file APIs accept beyond MAX_PATH; other platforms keep the path as is.
inline std::filesystem::path extendedPath(const std::filesystem::path& path) {
#ifdef _WIN32
    if (path.native().size() < extendedPathThreshold || path.native().starts_with(extendedPrefix) || !path.has_root_name())
        return path;
    // The extended form turns off the normalization of the system: separators and dot segments are resolved here.
    auto native = std::filesystem::absolute(path).lexically_normal().make_preferred().native();
    if (native.starts_with(networkPrefix))
        native = std::wstring(uncPrefix) + native.substr(2);
    else
        native = std::wstring(extendedPrefix) + native;
    return native;
#else
    return path;
#endif
}

/// The ordinary spelling of a path, so that the extended and the ordinary forms of one location compare equal.
inline std::filesystem::path plainPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& native = path.native();
    if (native.starts_with(uncPrefix))
        return std::wstring(networkPrefix) + native.substr(uncPrefix.size());
    if (native.starts_with(extendedPrefix))
        return native.substr(extendedPrefix.size());
#endif
    return path;
}

/// The canonical absolute path of an existing project directory, in the extended-length form when it is long:
/// canonical() of the system functions refuses the ordinary spelling beyond MAX_PATH.
inline std::filesystem::path canonicalDirectory(const std::filesystem::path& directory) {
    return extendedPath(plainPath(std::filesystem::canonical(extendedPath(std::filesystem::absolute(directory)))));
}

// Resolve existing parents too: a symlink must never turn a relative project path into an escape.
inline std::filesystem::path projectPath(const std::filesystem::path& root, std::string_view relative) {
    const auto path      = extendedPath(root / fs::safeRelativePath(relative));
    const auto resolved  = plainPath(std::filesystem::weakly_canonical(path));
    const auto plainRoot = plainPath(root);
    auto       base      = plainRoot.begin();
    auto       part      = resolved.begin();
    for (; base != plainRoot.end(); ++base, ++part)
        if (part == resolved.end() || *part != *base)
            throw std::invalid_argument("Path escapes the developer project");
    return path;
}

inline bool isParent(const std::filesystem::path& parentPath, const std::filesystem::path& childPath) {
    const auto parent = plainPath(parentPath);
    const auto child  = plainPath(childPath);
    auto       part   = child.begin();
    for (const auto& component : parent) {
        if (part == child.end() || *part != component)
            return false;
        ++part;
    }
    return true;
}

inline bool related(const std::filesystem::path& first, const std::filesystem::path& second) {
    return isParent(first, second) || isParent(second, first);
}

}  // namespace webfront::developer
