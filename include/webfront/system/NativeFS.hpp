/// @date 26/04/2023 16:39:42
/// @author Ambroise Leclerc
/// @brief Native file system providing direct access to the operating system's file system.
#pragma once

#include "FileSystem.hpp"
#include "FontStacks.hpp"

#include <fstream>
#include <system_error>

namespace webfront::fs {

namespace detail {
/// @brief Low-level implementation of a native file system.
class NativeRawFS {
public:
    std::optional<File> open(std::filesystem::path path) {
        std::ifstream file;
        file.open(rootPath / path, std::ios::binary);
        if (file.is_open())
            return File{std::move(file)};
        return {};
    }

protected:
    std::filesystem::path rootPath;
};
}  // namespace detail

/// @brief Native file system for debugging purposes.
class NativeDebugFS : public detail::NativeRawFS {
public:
    explicit NativeDebugFS(std::filesystem::path docRoot) {
        rootPath = docRoot;
    }

    /// Opens like the raw file system and, for a stylesheet, prints a hint once when a `font-family` lists many families
    /// (#611, FontStacks.hpp). The development path only: a stylesheet over 1 MiB is not read, and one already checked is
    /// read again only after its modification time or size changes.
    std::optional<File> open(std::filesystem::path path) {
        auto file = NativeRawFS::open(path);
        if (file && path.extension() == ".css") {
            try {
                // Keyed by the full path: two roots may each serve a style.css.
                const auto      full = (rootPath / path).generic_string();
                std::error_code sizeError, timeError;
                const auto      bytes   = std::filesystem::file_size(rootPath / path, sizeError);
                const auto      written = std::filesystem::last_write_time(rootPath / path, timeError);
                // The stamp keeps the file clock's own representation, and only after both reads succeeded.
                if (sizeError || timeError || bytes > 1024 * 1024)
                    return file;
                const fonts::Stamp stamp{bytes, written};
                if (fonts::unchecked(full, stamp)) {
                    std::ifstream css(rootPath / path, std::ios::binary);
                    if (!css.is_open())
                        return file;
                    // Keep the diagnostic read within the checked size even if the file grows. A counted read also
                    // avoids GCC 13's null-dereference false positive in an inlined istreambuf_iterator.
                    std::string text(static_cast<std::size_t>(bytes), '\0');
                    css.read(text.data(), static_cast<std::streamsize>(text.size()));
                    text.resize(static_cast<std::size_t>(css.gcount()));
                    fonts::warnLongStacks(full, text, stamp);
                }
            } catch (...) {  // a hint must never fail a request
            }
        }
        return file;
    }
};

}  // namespace webfront::fs
