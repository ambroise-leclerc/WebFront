/// @date 26/04/2023 16:39:42
/// @author Ambroise Leclerc
/// @brief Native file system providing direct access to the operating system's file system.
#pragma once

#include "FileSystem.hpp"
#include "FontStacks.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

namespace webfront::fs {

namespace detail {
/// @brief Low-level implementation of a native file system.
class NativeRawFS {
public:
    std::optional<File> open(std::filesystem::path path) {
        std::ifstream file;
        file.open(rootPath / path, std::ios::binary);
        if (file.is_open()) return File{std::move(file)};
        return {};
    }
protected:
    std::filesystem::path rootPath;
};
} // namespace detail

/// @brief Native file system for debugging purposes.
class NativeDebugFS : public detail::NativeRawFS {
public:
    explicit NativeDebugFS(std::filesystem::path docRoot) {
        rootPath = docRoot;
    }

    /// Opens like the raw file system and, for a stylesheet, prints a hint once when a `font-family` lists many families
    /// (#611, FontStacks.hpp). The development path only: a stylesheet over 1 MiB is not read, and one already checked is
    /// read again only after its modification time or size changes.
    std::optional<File> open(std::filesystem::path path) {
        auto file = NativeRawFS::open(path);
        if (file && path.extension() == ".css") {
            try {
                // Keyed by the full path: two roots may each serve a style.css.
                const auto        full = (rootPath / path).generic_string();
                std::error_code   sizeError, timeError;
                const auto        bytes   = std::filesystem::file_size(rootPath / path, sizeError);
                const auto        written = std::filesystem::last_write_time(rootPath / path, timeError);
                const std::string stamp = std::to_string(bytes) + ":" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(written.time_since_epoch()).count());
                if (!sizeError && !timeError && bytes <= 1024 * 1024 && fonts::unchecked(full, stamp)) {
                    std::ifstream css(rootPath / path, std::ios::binary);
                    std::string   text(std::istreambuf_iterator<char>(css), {});
                    fonts::warnLongStacks(full, text, stamp);
                }
            } catch (...) {  // a hint must never fail a request
            }
        }
        return file;
    }
};

} // namespace webfront::fs