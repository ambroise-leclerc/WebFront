/// @brief Locally served, pinned Babel assets; see docs/EMBEDDED_ASSETS.md.
#pragma once

#include "BabelData.hpp"
#include "FileSystem.hpp"

namespace webfront::fs {

class BabelFS {
public:
    explicit BabelFS(std::filesystem::path /*docRoot*/) { }

    static std::optional<File> open(std::filesystem::path file) {
        const auto filename = file.relative_path().generic_string();
        if (filename == "babel-standalone@8.0.7/babel.min.js")
            return File{generated::BabelData{}};
        return {};
    }
};

}  // namespace webfront::fs
