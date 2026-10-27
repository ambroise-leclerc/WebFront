/// @date 26/04/2023 22:39:42
/// @author Ambroise Leclerc
/// @brief Virtual file system providing access to WebFront.js, favicon.ico and a minimal index.html.
#pragma once

#include "DomInterpreterData.hpp"
#include "FallbackIndexHtmlData.hpp"
#include "FallbackModuleData.hpp"
#include "FaviconData.hpp"
#include "FileSystem.hpp"
#include "WebFrontJsData.hpp"

#include <filesystem>

namespace webfront::fs {

class IndexFS {
public:
    IndexFS(std::filesystem::path /*docRoot*/) {}
    IndexFS() = delete;
    IndexFS(const IndexFS&) = default;
    IndexFS(IndexFS&&) = default;
    IndexFS& operator=(const IndexFS&) = default;
    IndexFS& operator=(IndexFS&&) = default;

    using IndexHtml = generated::FallbackIndexHtmlData;
    using FallbackModule = generated::FallbackModuleData;

    using WebFrontIco = generated::FaviconData;

    using WebFrontJs = generated::WebFrontJsData;

    static std::optional<File> open(std::filesystem::path file) {
        auto filename = file.relative_path().string();
        if (filename == "index.html") return File(IndexHtml{});
        if (filename == "favicon.ico") return File{WebFrontIco{}};
        if (filename == "webfront-dom.mjs") return File{generated::DomInterpreterData{}};
        if (filename == "WebFront.js") return File{WebFrontJs{}};
        if (filename == "webfront-fallback.mjs") return File{FallbackModule{}};
        return {};
    }
};

} // namespace webfront::fs
