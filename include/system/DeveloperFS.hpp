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
        if (auto embedded = embeddedResource(path))
            return embedded;
        constexpr std::string_view prefix = "webfront/developer/";
        if (!root.empty() && path.starts_with(prefix)) {
            const auto relative = std::string_view(path).substr(prefix.size());
            // Only published resources, never source files, staging or the cached archive.
            if (!relative.starts_with("environment/") && !relative.starts_with("preview/"))
                return {};
            if (auto published = openPublished(relative))
                return published;
        }
        return IndexFS::open(file);
    }

private:
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
            std::ifstream stream(target, std::ios::binary);
            if (stream)
                return File{std::move(stream)};
        } catch (const std::exception&) { }
        return std::nullopt;
    }

    std::filesystem::path root;
};

}  // namespace webfront::fs
