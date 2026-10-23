/// @brief Path containment helpers of the developer store.
#pragma once

#include "SafePath.hpp"

#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace webfront::developer {

// Resolve existing parents too: a symlink must never turn a relative project path into an escape.
inline std::filesystem::path projectPath(const std::filesystem::path& root, std::string_view relative) {
    const auto path     = root / fs::safeRelativePath(relative);
    const auto resolved = std::filesystem::weakly_canonical(path);
    auto       base     = root.begin();
    auto       part     = resolved.begin();
    for (; base != root.end(); ++base, ++part)
        if (part == resolved.end() || *part != *base)
            throw std::invalid_argument("Path escapes the developer project");
    return path;
}

inline bool isParent(const std::filesystem::path& parent, const std::filesystem::path& child) {
    auto part = child.begin();
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
