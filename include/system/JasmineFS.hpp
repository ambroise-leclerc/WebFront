/// @brief Locally served, pinned Jasmine assets; see docs/EMBEDDED_ASSETS.md.
#pragma once

#include "FileSystem.hpp"
#include "JasmineBootData.hpp"
#include "JasmineCssData.hpp"
#include "JasmineFaviconData.hpp"
#include "JasmineHtmlData.hpp"
#include "JasmineJsData.hpp"

namespace webfront::fs {

class JasmineFS {
public:
    explicit JasmineFS(std::filesystem::path /*docRoot*/) { }

    static std::optional<File> open(std::filesystem::path file) {
        const auto filename = file.relative_path().generic_string();
        if (filename == "jasmine/7.0.2/jasmine.js")
            return File{generated::JasmineJsData{}};
        if (filename == "jasmine/7.0.2/jasmine-html.js")
            return File{generated::JasmineHtmlData{}};
        if (filename == "jasmine/7.0.2/jasmine.css")
            return File{generated::JasmineCssData{}};
        if (filename == "jasmine/7.0.2/boot.js")
            return File{generated::JasmineBootData{}};
        if (filename == "jasmine/7.0.2/jasmine_favicon.png")
            return File{generated::JasmineFaviconData{}};
        return {};
    }
};

}  // namespace webfront::fs
