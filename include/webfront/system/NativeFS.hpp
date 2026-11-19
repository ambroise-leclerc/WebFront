/// @date 26/04/2023 16:39:42
/// @author Ambroise Leclerc
/// @brief Native file system providing direct access to the operating system's file system.
#pragma once

#include "FileSystem.hpp"
#include "FontStacks.hpp"
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
    /// (#611, FontStacks.hpp). The development path only: a stylesheet over 1 MiB is not read, and a stylesheet already
    /// checked is not read again.
    std::optional<File> open(std::filesystem::path path) {
        auto file = NativeRawFS::open(path);
        if (file && path.extension() == ".css" && fonts::unchecked(path.generic_string())) {
            try {
                std::error_code size;
                const auto      bytes = std::filesystem::file_size(rootPath / path, size);
                if (!size && bytes <= 1024 * 1024) {
                    std::ifstream css(rootPath / path, std::ios::binary);
                    std::string   text(std::istreambuf_iterator<char>(css), {});
                    fonts::warnLongStacks(path.generic_string(), text);
                }
            } catch (...) {  // a hint must never fail a request
            }
        }
        return file;
    }
};

} // namespace webfront::fs