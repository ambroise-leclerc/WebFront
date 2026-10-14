/// @brief Virtual file system serving the terminal component's page, module and the vendored
/// xterm.js distribution (MIT, see third_party/xterm/LICENSE) under webfront/components/terminal/.
#pragma once

#include "FileSystem.hpp"
#include "TerminalIndexHtmlData.hpp"
#include "TerminalModuleData.hpp"
#include "XtermCssData.hpp"
#include "XtermFitAddonData.hpp"
#include "XtermModuleData.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace webfront::fs {

class TerminalFS {
public:
    static constexpr std::string_view prefix = "webfront/components/terminal/";
    static constexpr std::string_view page   = "webfront/components/terminal/index.html";

    explicit TerminalFS(std::filesystem::path /*docRoot*/) {}

    static std::optional<File> open(std::filesystem::path file) {
        const auto filename = file.relative_path().generic_string();
        if (!filename.starts_with(prefix))
            return {};
        const auto name = std::string_view(filename).substr(prefix.size());
        if (name == "index.html")
            return File{generated::TerminalIndexHtmlData{}};
        if (name == "terminal.mjs")
            return File{generated::TerminalModuleData{}};
        if (name == "xterm.mjs")
            return File{generated::XtermModuleData{}};
        if (name == "xterm.css")
            return File{generated::XtermCssData{}};
        if (name == "addon-fit.mjs")
            return File{generated::XtermFitAddonData{}};
        return {};
    }
};

}  // namespace webfront::fs
