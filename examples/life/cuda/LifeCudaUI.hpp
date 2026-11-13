#pragma once

// The page of the demo, embedded in the executable. The two headers are rendered from ui/ at build time by
// js/core/EmbedAsset.cmake (see cmake/LifeCuda.cmake): the page is plain HTML and JavaScript, so no wf step and
// no network access stand between the sources and the binary.
#include <LifeCudaIndexHtmlData.hpp>
#include <LifeCudaModuleData.hpp>

#include <webfront/system/FileSystem.hpp>

#include <filesystem>
#include <optional>

namespace webfront::demo::lifecuda {

class LifeCudaUI {
public:
    LifeCudaUI(std::filesystem::path /*docRoot*/) {}

    static std::optional<fs::File> open(std::filesystem::path file) {
        const auto filename = file.relative_path().string();
        if (filename == "index.html")
            return fs::File{fs::generated::LifeCudaIndexHtmlData{}};
        if (filename == "life-cuda.mjs")
            return fs::File{fs::generated::LifeCudaModuleData{}};
        return {};
    }
};

}  // namespace webfront::demo::lifecuda
