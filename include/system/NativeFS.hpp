/// @date 26/04/2022 16:39:42
/// @author Ambroise Leclerc
/// @brief Virtual file system providing access to Jasmine Javascript testing framework.
#pragma once

#include "FileSystem.hpp"

#include <fstream>

namespace webfront::fs {

namespace detail {
class NativeRawFS {
public:
    static std::optional<std::ifstream> open(std::filesystem::path path) {
        std::ifstream file;
        file.open(path, std::ios::binary);
        return file;
    }
};
} // namespace

class NativeDebugFS : public detail::NativeRawFS {
public:
    NativeDebugFS(std::filesystem::path /*rootPath*/) {

    }

};
} // namespace webfront::fs