/// @brief Virtual file system serving the browser side of the wf tool manager (the terminal extension
/// wf.mjs) under webfront/components/tools/.
#pragma once

#include "FileSystem.hpp"
#include "ToolManagerModuleData.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace webfront::fs {

class ToolManagerFS {
public:
    static constexpr std::string_view prefix = "webfront/components/tools/";
    static constexpr std::string_view module = "webfront/components/tools/wf.mjs";

    explicit ToolManagerFS(std::filesystem::path /*docRoot*/) {}

    static std::optional<File> open(std::filesystem::path file) {
        if (file.relative_path().generic_string() == module)
            return File{generated::ToolManagerModuleData{}};
        return {};
    }
};

}  // namespace webfront::fs
