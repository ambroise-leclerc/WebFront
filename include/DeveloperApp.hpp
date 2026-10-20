/// @brief Opt-in browser-powered developer environment. Downloaded code has project/bridge authority.
#pragma once

#include "system/DeveloperFS.hpp"
#include "WebFront.hpp"

namespace webfront {

struct DeveloperBootstrapOptions {
    /// HTTPS npm tarball URL, or same-origin URL for local provisioning/tests.
    std::string environment;
    /// Trusted SHA-512 SRI of that exact tarball, supplied independently of its download.
    std::string integrity;
    std::string registry{"https://registry.npmjs.org/"};
};

template <frontend::FrontendType Frontend = frontend::DefaultFrontend, fs::Provider... Extra>
class BasicDeveloperApp : public BasicWF<NetProvider, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend> {
    using Base = BasicWF<NetProvider, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend>;

public:
    /// An explicit project is required before enabling development file operations.
    explicit BasicDeveloperApp(std::filesystem::path project = {}, WebFrontConfig config = {})
        : Base(withProject(config, project)), project(project.empty() ? std::filesystem::path{} : std::filesystem::canonical(project)) { }

    void bootstrap(std::string_view mode, DeveloperBootstrapOptions options) {
        if (mode != "developer")
            throw std::invalid_argument("Unknown bootstrap mode");
        if (store)
            throw std::logic_error("Developer bootstrap is already configured");
        if (project.empty())
            throw std::invalid_argument("Developer bootstrap requires an explicit project directory");
        if (options.environment.empty() || !options.integrity.starts_with("sha512-"))
            throw std::invalid_argument("Developer bootstrap requires an environment URL and trusted SHA-512 integrity");
        const auto config = "{\"environment\":" + json::quote(options.environment) + ",\"integrity\":" + json::quote(options.integrity)
                            + ",\"registry\":" + json::quote(options.registry) + "}";
        store             = std::make_shared<developer::Store>(project);
        this->cppFunction("webfront.developer.config", [config] {
            return config;
        });
        this->cppFunction("webfront.developer.read", [owned = store](std::string path, std::uint64_t offset) {
            return owned->read(std::move(path), offset);
        });
        this->cppFunction("webfront.developer.begin", [owned = store](std::string destination, bool directory) {
            return owned->begin(std::move(destination), directory);
        });
        this->cppFunction("webfront.developer.write", [owned = store](std::string id, std::string path, std::uint64_t offset, std::vector<std::uint8_t> bytes) {
            owned->write(std::move(id), std::move(path), offset, std::move(bytes));
        });
        this->cppFunction("webfront.developer.commit", [owned = store](std::string id) {
            owned->commit(std::move(id));
        });
        this->cppFunction("webfront.developer.abort", [owned = store](std::string id) {
            owned->abort(std::move(id));
        });
    }

    void show(std::string_view page = {}) {
        Base::show(page.empty() ? (store ? "webfront-bootstrap.html" : "index.html") : page);
    }

private:
    static WebFrontConfig withProject(WebFrontConfig config, const std::filesystem::path& project) {
        config.documentRoot = project;
        return config;
    }
    std::filesystem::path             project;
    std::shared_ptr<developer::Store> store;
};

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
using NativeApp = BasicDeveloperApp<frontend::NativeWebViewFrontend>;

/// Distribution form: generated resources, protected bridge and fallback; no developer file API.
template <fs::Provider... Assets>
using NativeAppWithAssets = BasicWF<NetProvider, fs::DefaultFSWith<Assets...>, http::DefaultBuffersPolicy, frontend::NativeWebViewFrontend>;
#endif

}  // namespace webfront
