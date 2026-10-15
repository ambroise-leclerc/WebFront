/// @brief Virtual file system serving the web tools installed by the wf tool manager under
/// webfront/tools/<name>/, from the versions recorded in the project's lock file (see ToolStore).
#pragma once

#include "FileSystem.hpp"
#include "ToolManagerModuleData.hpp"
#include "ToolStore.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace webfront::fs {

namespace detail {

inline std::string encodePath(std::string_view path) {
    constexpr std::string_view kept      = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/@";
    constexpr std::string_view hexDigits = "0123456789ABCDEF";
    std::string                encoded;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (kept.find(c) != std::string_view::npos)
            encoded += c;
        else
            encoded += {'%', hexDigits[byte >> 4], hexDigits[byte & 0xF]};
    }
    return encoded;
}

}  // namespace detail

/// Default project location of the installed tools: the process's working directory when the
/// application starts.
struct CurrentDirectory {
    static std::filesystem::path get() {
        return std::filesystem::current_path();
    }
};

/// Serves webfront/tools/<name>/<path> from the installed version of <name>, and the tool manager's
/// browser module (src/components/tools/wf.mjs) at toolManagerModule. The lock file is read on each
/// request, so tools installed or removed while the application runs are served at once.
/// Location::get() gives the project directory; the ToolManager component must use the same one.
/// Development only: an installed tool runs in the application's origin.
template <typename Location = CurrentDirectory>
class BasicPackageFS {
public:
    static constexpr std::string_view prefix            = "webfront/tools/";
    static constexpr std::string_view toolManagerModule = "/webfront/components/tools/wf.mjs";

    explicit BasicPackageFS(std::filesystem::path /*docRoot*/) : project(Location::get()) {}

    /// Absolute URL path of a file of an installed tool, such as the page of its entry. Characters
    /// other than unreserved ones, '/' and '@' are percent-encoded, so the server decodes it back.
    static std::string url(std::string_view name, std::string_view path) {
        return "/" + std::string(prefix) + detail::encodePath(name) + "/" + detail::encodePath(path);
    }

    std::optional<File> open(std::filesystem::path file) const {
        const auto filename = file.relative_path().generic_string();
        if (filename == toolManagerModule.substr(1))
            return File{generated::ToolManagerModuleData{}};
        if (!filename.starts_with(prefix))
            return {};
        const auto rest = std::string_view(filename).substr(prefix.size());
        // A scoped name ("@scope/name") spans two components.
        const auto nameEnd = rest.find('/', rest.starts_with('@') ? rest.find('/') + 1 : 0);
        if (nameEnd == std::string_view::npos)
            return {};
        try {
            return openToolFile(rest.substr(0, nameEnd), rest.substr(nameEnd + 1));
        } catch (const std::exception&) {  // malformed lock file or unsafe path: not served
            return {};
        }
    }

private:
    std::optional<File> openToolFile(std::string_view name, std::string_view relative) const {
        const ToolStore store{project};
        const auto      record = store.find(name);
        if (!record)
            return {};
        const auto path = store.installDir(record->name, record->version) / tools::safeRelativePath(relative);
        if (!std::filesystem::is_regular_file(path))
            return {};
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
            return {};
        return File{std::move(stream)};
    }

    std::filesystem::path project;
};

using PackageFS = BasicPackageFS<>;

}  // namespace webfront::fs
