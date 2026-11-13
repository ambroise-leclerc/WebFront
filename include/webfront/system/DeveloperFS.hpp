/// @brief Bootstrap assets and the explicit developer project's published browser resources.
#pragma once

#include "DeveloperBootstrapData.hpp"
#include "DeveloperIndexHtmlData.hpp"
#include "DeveloperStore.hpp"
#include "IndexFS.hpp"
#ifdef _WIN32
#include "WindowsCompat.hpp"
#endif

#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <algorithm>
#include <memory>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>

namespace webfront::fs {

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
    // Publication replaces directories atomically. An HTTP response owns its old file while sending it, but must not
    // prevent the directory rename or removal: a slow browser can retain that handle beyond any bounded retry.
    class PublishedSource final : public ByteSource {
    public:
        PublishedSource(HANDLE opened, std::uint64_t length) : handle(opened), total(length) {}
        PublishedSource(const PublishedSource&) = delete;
        ~PublishedSource() override { ::CloseHandle(handle); }
        std::size_t read(std::span<char> buffer) override {
            std::size_t count = 0;
            while (count < buffer.size()) {
                DWORD got = 0;
                const auto want = static_cast<DWORD>(std::min<std::size_t>(buffer.size() - count, 1u << 30));
                if (!::ReadFile(handle, buffer.data() + count, want, &got, nullptr))
                    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "Cannot read published developer resource");
                if (got == 0) break;
                count += got;
            }
            return count;
        }
        std::optional<std::uint64_t> size() const override { return total; }
        bool seek(std::uint64_t offset) override {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(std::min(offset, total));
            return ::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != 0;
        }
    private:
        HANDLE handle;
        std::uint64_t total;
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
            const auto    target = developer::projectPath(root, ".webfront/developer/" + std::string(relative));
#ifdef _WIN32
            const auto handle = ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
            auto close = [](void* value) { ::CloseHandle(value); };
            std::unique_ptr<void, decltype(close)> opened(handle, close);
            LARGE_INTEGER length{};
            if (!::GetFileSizeEx(handle, &length) || length.QuadPart < 0) return std::nullopt;
            auto source = std::make_unique<PublishedSource>(handle, static_cast<std::uint64_t>(length.QuadPart));
            opened.release();
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
