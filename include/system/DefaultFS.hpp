/// @brief Default filesystem for webfront::WebFront: embedded bridge script, an optional local
/// development root, and an embedded fallback page - composed with the precedence documented below.
#pragma once

#include "FileSystem.hpp"
#include "IndexFS.hpp"

#include <filesystem>
#include <fstream>

namespace webfront::fs {

namespace detail {

/// Never touches disk: always serves the embedded, version-matched bridge script and nothing else.
/// Listed first in DefaultFS so a configured document root can never shadow it.
class ProtectedWebFrontJsFS {
public:
    explicit ProtectedWebFrontJsFS(std::filesystem::path /*docRoot*/) {}

    static std::optional<File> open(std::filesystem::path file) {
        if (file.relative_path().string() == "WebFront.js") return File{IndexFS::WebFrontJs{}};
        return {};
    }
};

/// Serves files under an explicitly configured root. When the root is empty (the default, meaning
/// "no document root configured"), open() always returns nullopt without touching the filesystem.
class OptionalDevRootFS {
public:
    explicit OptionalDevRootFS(std::filesystem::path docRoot) : root(std::move(docRoot)) {}

    std::optional<File> open(std::filesystem::path file) const {
        if (root.empty()) return {};
        std::ifstream stream(root / file, std::ios::binary);
        if (stream.is_open()) return File{std::move(stream)};
        return {};
    }

private:
    std::filesystem::path root;
};

} // namespace detail

/// Precedence (Multi<> serves the first provider that returns a file):
///  1. The embedded, version-matched WebFront.js - always wins, never overridable.
///  2. Files under an explicitly configured document root - development only; no disk access at
///     all when unconfigured.
///  3. IndexFS's embedded fallback page, favicon, and WebFront.js copy.
using DefaultFS = Multi<detail::ProtectedWebFrontJsFS, detail::OptionalDevRootFS, IndexFS>;

} // namespace webfront::fs
