#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace webfront::frontend::detail {

/**
 * @brief A path in `folder` that does not exist yet, for a download whose name the page suggested.
 *
 * Only the final component of the suggested name is kept (a page can neither choose a folder nor climb out of it), an
 * empty name becomes "download", and an existing file is kept: "name", then "name (1).ext", "name (2).ext", and so on.
 */
inline std::filesystem::path uniqueDownloadPath(const std::filesystem::path& folder, std::string_view suggested) {
    auto name = std::filesystem::path(std::string(suggested)).filename();
    if (name.empty() || name == "." || name == "..")
        name = "download";
    auto            result = folder / name;
    std::error_code error;
    for (int index = 1; std::filesystem::exists(result, error) && index < 10'000; ++index)
        result = folder / (name.stem().string() + " (" + std::to_string(index) + ")" + name.extension().string());
    return result;
}

}  // namespace webfront::frontend::detail
