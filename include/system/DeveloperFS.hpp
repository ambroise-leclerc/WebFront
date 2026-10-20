/// @brief Bootstrap assets and the explicit developer project's published browser resources.
#pragma once

#include "DeveloperBootstrapData.hpp"
#include "DeveloperIndexHtmlData.hpp"
#include "DeveloperStore.hpp"
#include "IndexFS.hpp"

namespace webfront::fs {

class DeveloperFS {
public:
    explicit DeveloperFS(std::filesystem::path project) {
        if (!project.empty())
            root = std::filesystem::canonical(project);
    }

    std::optional<File> open(std::filesystem::path file) const {
        const auto path = file.relative_path().generic_string();
        if (path == "WebFront.js")
            return File{IndexFS::WebFrontJs{}};
        if (path == "webfront-bootstrap.html")
            return File{generated::DeveloperIndexHtmlData{}};
        if (path == "webfront-bootstrap.mjs")
            return File{generated::DeveloperBootstrapData{}};
        constexpr std::string_view prefix = "webfront/developer/";
        if (!root.empty() && path.starts_with(prefix)) {
            try {
                const auto relative = std::string_view(path).substr(prefix.size());
                // Only published resources, never source files, staging or the cached archive.
                if (!relative.starts_with("environment/") && !relative.starts_with("preview/"))
                    return {};
                const auto    target = developer::projectPath(root, ".webfront/developer/" + std::string(relative));
                std::ifstream stream(target, std::ios::binary);
                if (stream)
                    return File{std::move(stream)};
            } catch (const std::exception&) { }
        }
        return IndexFS::open(file);
    }

private:
    std::filesystem::path root;
};

}  // namespace webfront::fs
