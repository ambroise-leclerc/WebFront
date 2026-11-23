// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-02
//
// WebFront — frontend
// Role: Locates the CEF framework relative to the executable on macOS.
// Thread: ui-loop | Alloc: heap-init
// LLM: small path helper used by CEF.hpp

#pragma once

#include <filesystem>

namespace webfront::cef::detail {

// Keep this layout aligned with copy_cef_runtime_files() and CefScopedLibraryLoader on macOS.
inline std::filesystem::path macOSFrameworkPath(const std::filesystem::path& executable) {
    return executable.parent_path().parent_path() / "Frameworks" / "Chromium Embedded Framework.framework";
}

}  // namespace webfront::cef::detail
