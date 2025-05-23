/// @date 26/04/2023 16:39:42
/// @author Ambroise Leclerc
/// @brief Native file system providing direct access to the operating system's file system.
#pragma once

#include "FileSystem.hpp"
#include <fstream>

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
};

} // namespace webfront::fs