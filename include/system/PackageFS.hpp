/// @brief Virtual file system serving the web tools installed by the wf tool manager under
/// webfront/tools/<name>/, from the versions recorded in the project's lock file (see ToolStore).
#pragma once

#include "FileSystem.hpp"
#include "ToolStore.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace webfront::fs {

/// Default project location of the installed tools: the process's working directory when the
/// application starts.
struct CurrentDirectory {
    static std::filesystem::path get() {
        return std::filesystem::current_path();
    }
};

/// Serves webfront/tools/<name>/<path> from the installed version of <name>. The lock file is read
/// on each request, so tools installed or removed while the application runs are served at once.
/// Location::get() gives the project directory; the ToolManager component must use the same one.
/// Development only: an installed tool runs in the application's origin.
template <typename Location = CurrentDirectory>
class BasicPackageFS {
public:
    static constexpr std::string_view prefix = "webfront/tools/";

    explicit BasicPackageFS(std::filesystem::path /*docRoot*/) : project(Location::get()) {}

    /// URL path of a file of an installed tool, such as the page of its entry.
    static std::string url(std::string_view name, std::string_view path) {
        return std::string(prefix) + std::string(name) + "/" + std::string(path);
    }

    std::optional<File> open(std::filesystem::path file) const {
        const auto filename = file.relative_path().generic_string();
        if (!filename.starts_with(prefix))
            return {};
        const auto rest = std::string_view(filename).substr(prefix.size());
        // A scoped name ("@scope/name") spans two components.
        const auto nameEnd = rest.find('/', rest.starts_with('@') ? rest.find('/') + 1 : 0);
        if (nameEnd == std::string_view::npos)
            return {};
        try {
            const ToolStore store{project};
            const auto      record = store.find(rest.substr(0, nameEnd));
            if (!record)
                return {};
            const auto path = store.installDir(record->name, record->version) / tools::safeRelativePath(rest.substr(nameEnd + 1));
            if (!std::filesystem::is_regular_file(path))
                return {};
            std::ifstream stream(path, std::ios::binary);
            if (stream.is_open())
                return File{std::move(stream)};
        } catch (const std::exception&) {  // malformed lock file or unsafe path: not served
        }
        return {};
    }

private:
    std::filesystem::path project;
};

using PackageFS = BasicPackageFS<>;

}  // namespace webfront::fs
