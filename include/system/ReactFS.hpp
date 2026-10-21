/// @brief Locally served, pinned React assets; see docs/EMBEDDED_ASSETS.md.
#pragma once

#include "FileSystem.hpp"
#include "ReactRuntimeData.hpp"

namespace webfront::fs {

class ReactFS {
public:
    explicit ReactFS(std::filesystem::path /*docRoot*/) { }

    static std::optional<File> open(std::filesystem::path file) {
        const auto filename = file.relative_path().generic_string();
        if (filename == "react@19.3.1/runtime.mjs")
            return File{generated::ReactRuntimeData{}};
        return {};
    }
};

}  // namespace webfront::fs
