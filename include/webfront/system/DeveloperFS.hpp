/// @brief Bootstrap assets and the explicit developer project's published browser resources.
#pragma once

#include "DeveloperBootstrapData.hpp"
#include "DeveloperIndexHtmlData.hpp"
#include "DeveloperStore.hpp"
#include "IndexFS.hpp"
#ifdef _WIN32
    #include "WindowsCompat.hpp"
#endif

#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace webfront::fs {

#ifdef _WIN32
namespace detail {
/// Copies `count` bytes that may sit in a mapped view whose backing storage can disappear (a network share dropped, a
/// removable medium ejected, a file truncated in place). Touching such a page raises EXCEPTION_IN_PAGE_ERROR, a
/// structured exception that C++ handlers do not see and that only MSVC can catch with __try. ReadProcessMemory on this
/// process copies with the kernel's own fault handling instead: it reports the failure by value on every Windows toolchain,
/// with no exception to catch. Returns false when any byte could not be read.
[[nodiscard]] inline bool copyReadable(void* destination, const void* source, std::size_t count) noexcept {
    SIZE_T copied = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), source, destination, count, &copied) != 0 && copied == count;
}
}  // namespace detail
#endif

class DeveloperFS {
public:
    explicit DeveloperFS(std::filesystem::path project) {
        if (!project.empty())
            root = developer::canonicalDirectory(project);
    }

    std::optional<File> open(std::filesystem::path file) const {
        const auto path = file.relative_path().generic_string();
        if (auto embedded = embeddedResource(path))
            return embedded;
        constexpr std::string_view prefix = "webfront/developer/";
        if (!root.empty() && path.starts_with(prefix)) {
            const auto relative = std::string_view(path).substr(prefix.size());
            // Only published resources, never source files, staging or the cached archive.
            if (!relative.starts_with("environment/") && !relative.starts_with("preview/") && !relative.starts_with("test/"))
                return {};
            if (auto published = openPublished(relative))
                return published;
        }
        return IndexFS::open(file);
    }

private:
#ifdef _WIN32
    // Windows refuses to rename a directory with an open child file, even with FILE_SHARE_DELETE. Retain a section
    // instead of the file handle and map only the read window: slow HTTP clients neither lock publication nor pin
    // the whole asset in process memory. The original section keeps serving the old publication after replacement.
    //
    // Contract: a published file is immutable. A publication replaces its directory; it never truncates or rewrites a
    // file in place. A page error while copying (see copyMapped) fails that read with std::system_error and so ends
    // only the response, on every toolchain. Each read maps one window, so the HTTP layer should ask for large blocks: the cost of a
    // read is a map/unmap pair, not a per-byte price.
    class PublishedSource final : public ByteSource {
    public:
        static bool copyMapped(void* destination, const void* source, std::size_t count) noexcept {
            return detail::copyReadable(destination, source, count);
        }

        PublishedSource(HANDLE opened, std::uint64_t length) : handle(opened), total(length) {
            SYSTEM_INFO info{};
            ::GetSystemInfo(&info);
            granularity = info.dwAllocationGranularity;
        }
        PublishedSource(const PublishedSource&) = delete;
        ~PublishedSource() override {
            if (handle)
                ::CloseHandle(handle);
        }
        std::size_t read(std::span<char> buffer) override {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), total - position));
            if (count == 0)
                return 0;
            const auto offset = position / granularity * granularity;
            const auto skip   = static_cast<std::size_t>(position - offset);
            auto       view   = ::MapViewOfFile(handle, FILE_MAP_READ, static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), count + skip);
            if (!view)
                throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "Cannot map published resource");
            const bool copied = copyMapped(buffer.data(), static_cast<const char*>(view) + skip, count);
            ::UnmapViewOfFile(view);  // on every path, a failed copy included
            if (!copied)
                throw std::system_error(static_cast<int>(ERROR_READ_FAULT), std::system_category(), "Cannot read published resource");
            position += count;
            return count;
        }
        std::optional<std::uint64_t> size() const override {
            return total;
        }
        bool seek(std::uint64_t offset) override {
            position = std::min(offset, total);
            return true;
        }

    private:
        HANDLE        handle;
        std::uint64_t total;
        std::uint64_t position    = 0;
        std::uint64_t granularity = 0;
    };
#endif
    static std::optional<File> embeddedResource(const std::string& path) {
        if (path == "WebFront.js")
            return File{IndexFS::WebFrontJs{}};
        if (path == "webfront-bootstrap.html")
            return File{generated::DeveloperIndexHtmlData{}};
        if (path == "webfront-bootstrap.mjs")
            return File{generated::DeveloperBootstrapData{}};
        return std::nullopt;
    }

    std::optional<File> openPublished(std::string_view relative) const {
        try {
            const auto target = developer::projectPath(root, ".webfront/developer/" + std::string(relative));
#ifdef _WIN32
            const auto handle = ::CreateFileW(target.c_str(),
                                              GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL,
                                              nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return std::nullopt;
            auto close = [](void* value) {
                ::CloseHandle(value);
            };
            std::unique_ptr<void, decltype(close)> opened(handle, close);
            LARGE_INTEGER                          length{};
            if (!::GetFileSizeEx(handle, &length) || length.QuadPart < 0)
                return std::nullopt;
            const auto mapping = length.QuadPart == 0 ? nullptr : ::CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (length.QuadPart != 0 && !mapping)
                return std::nullopt;
            std::unique_ptr<void, decltype(close)> section(mapping, close);
            auto                                   source = std::make_unique<PublishedSource>(mapping, static_cast<std::uint64_t>(length.QuadPart));
            section.release();
            return File{std::move(source)};
#else
            std::ifstream stream(target, std::ios::binary);
            if (stream)
                return File{std::move(stream)};
#endif
        } catch (const std::exception&) { }
        return std::nullopt;
    }

    std::filesystem::path root;
};

}  // namespace webfront::fs
