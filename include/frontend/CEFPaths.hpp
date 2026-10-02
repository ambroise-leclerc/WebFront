#pragma once

#include <filesystem>

namespace webfront::cef::detail {

// Keep this layout aligned with copy_cef_runtime_files() and CefScopedLibraryLoader on macOS.
inline std::filesystem::path macOSFrameworkPath(const std::filesystem::path& executable) {
    return executable.parent_path().parent_path() / "Frameworks" / "Chromium Embedded Framework.framework";
}

}  // namespace webfront::cef::detail
