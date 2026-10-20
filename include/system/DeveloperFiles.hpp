/// @brief Path and file helpers of the developer store: project containment, staging and replacement.
#pragma once

#include "ToolStore.hpp"

#include <atomic>
#include <chrono>
#include <limits>

namespace webfront::developer {

inline constexpr std::size_t   chunkSize           = 256 * 1024;
inline constexpr std::uint64_t maxTransactionBytes = 256 * 1024 * 1024;
inline constexpr std::size_t   maxTransactions     = 8;
/// A transaction untouched for this long is abandoned: its browser reloaded or disconnected.
inline constexpr std::chrono::minutes defaultAbandonedAfter{10};
/// Staging left by a crashed process is removed at startup once it is this old.
inline constexpr std::chrono::hours staleStagingAfter{1};

// Resolve existing parents too: a symlink must never turn a relative project path into an escape.
inline std::filesystem::path projectPath(const std::filesystem::path& root, std::string_view relative) {
    const auto path     = root / fs::tools::safeRelativePath(relative);
    const auto resolved = std::filesystem::weakly_canonical(path);
    auto       base     = root.begin();
    auto       part     = resolved.begin();
    for (; base != root.end(); ++base, ++part)
        if (part == resolved.end() || *part != *base)
            throw std::invalid_argument("Path escapes the developer project");
    return path;
}

// Numbers the staging directories of every store in the process.
inline std::atomic<std::uint64_t> stagingCounter{0};

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

inline void seek(std::ifstream& file, std::uint64_t offset, const std::string& failure) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        throw std::invalid_argument("Invalid read offset");
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file)
        throw std::runtime_error(failure);
}

inline std::ifstream openForReading(const std::filesystem::path& source, const std::string& path, std::uint64_t offset) {
    const std::string failure = "Cannot read project file: " + path;
    if (!std::filesystem::is_regular_file(source))
        throw std::runtime_error(failure);
    std::ifstream file(source, std::ios::binary);
    if (!file)
        throw std::runtime_error(failure);
    seek(file, offset, "Cannot seek project file: " + path);
    return file;
}


inline std::pair<std::string, std::filesystem::path> createStaging(const std::filesystem::path& stagingRoot) {
    for (;;) {
        auto id        = std::to_string(++stagingCounter);
        auto directory = stagingRoot / id;
        if (std::filesystem::create_directory(directory))
            return {std::move(id), std::move(directory)};
    }
}

// Moves the staged files over the target, restoring the previous content if that fails.
inline void replaceTarget(const std::filesystem::path& target, bool replace, const std::filesystem::path& source, const std::filesystem::path& backup) {
    std::filesystem::create_directories(target.parent_path());
    // A backup left by a crashed process must not make the rename below fail.
    std::filesystem::remove_all(backup);
    const bool exists = std::filesystem::exists(std::filesystem::symlink_status(target));
    if (exists && !replace)
        throw std::runtime_error("Starter destination already exists");
    if (exists)
        std::filesystem::rename(target, backup);
    try {
        std::filesystem::rename(source, target);
    } catch (...) {
        if (exists)
            std::filesystem::rename(backup, target);
        throw;
    }
}

inline void removeStaleStaging(const std::filesystem::path& root) {
    std::error_code error;
    const auto      stagingRoot = root / ".webfront" / "developer" / "staging";
    const auto      now         = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(stagingRoot, error))
        if (const auto written = entry.last_write_time(error); !error && now - written >= staleStagingAfter)
            std::filesystem::remove_all(entry.path(), error);
}

}  // namespace webfront::developer
