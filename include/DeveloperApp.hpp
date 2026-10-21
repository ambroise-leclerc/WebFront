/// @brief Opt-in browser-powered developer environment. Downloaded code has project/bridge authority.
#pragma once

#include "system/DeveloperFS.hpp"
#include "system/DeveloperReleaseData.hpp"
#include "WebFront.hpp"

namespace webfront {

struct DeveloperBootstrapOptions {
    /// HTTPS npm tarball URL, or same-origin URL for local provisioning/tests.
    std::string environment;
    /// Trusted SHA-512 SRI of that exact tarball, supplied independently of its download.
    std::string integrity;
    std::string registry{"https://registry.npmjs.org/"};
};

namespace developer {
/// The project is the document root of the protected bridge. Its callbacks write the project, so a
/// WebSocket without an Origin header (not a browser page) is refused.
inline WebFrontConfig projectConfig(WebFrontConfig config, const std::filesystem::path& projectDirectory) {
    config.documentRoot                     = projectDirectory;
    config.contentPolicy.allowMissingOrigin = false;
    return config;
}

inline void validateBootstrap(std::string_view mode, const std::filesystem::path& project, const DeveloperBootstrapOptions& options) {
    if (mode != "developer")
        throw std::invalid_argument("Unknown bootstrap mode");
    if (project.empty())
        throw std::invalid_argument("Developer bootstrap requires an explicit project directory");
    if (options.environment.empty() || !options.integrity.starts_with("sha512-"))
        throw std::invalid_argument("Developer bootstrap requires an environment URL and trusted SHA-512 integrity");
}
}  // namespace developer

template <frontend::FrontendType Frontend = frontend::DefaultFrontend, fs::Provider... Extra>
class BasicDeveloperApp : public BasicWF<NetProvider, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend> {
    using Base = BasicWF<NetProvider, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend>;

public:
    /// An explicit project is required before enabling development file operations.
    explicit BasicDeveloperApp(std::filesystem::path projectDirectory = {}, WebFrontConfig config = {})
        : Base(developer::projectConfig(std::move(config), projectDirectory)),
          project(projectDirectory.empty() ? std::filesystem::path{} : std::filesystem::canonical(projectDirectory)) { }

    void bootstrap(std::string_view          mode,
                   DeveloperBootstrapOptions options = {.environment = std::string(developer::release::environment),
                                                        .integrity   = std::string(developer::release::integrity)}) {
        if (store)
            throw std::logic_error("Developer bootstrap is already configured");
        developer::validateBootstrap(mode, project, options);
        store = std::make_shared<developer::Store>(project);
        registerBridge("{\"environment\":" + json::quote(options.environment) + ",\"integrity\":" + json::quote(options.integrity)
                       + ",\"registry\":" + json::quote(options.registry) + "}");
    }

    void show(std::string_view page = {}) {
        Base::show(page.empty() ? (store ? "webfront-bootstrap.html" : "index.html") : page);
    }

private:
    void registerBridge(std::string config) {
        using Bytes = std::vector<std::uint8_t>;
        using developer::Store;
        this->cppFunction("webfront.developer.config", [config = std::move(config)] {
            return config;
        });
        expose<&Store::read, Bytes, std::string, std::uint64_t>("webfront.developer.read");
        expose<&Store::beginReplacing, std::string, std::string, bool>("webfront.developer.begin");
        expose<&Store::beginNew, std::string, std::string, bool>("webfront.developer.beginNew");
        expose<&Store::write, void, std::string, std::string, std::uint64_t, Bytes>("webfront.developer.write");
        expose<&Store::commit, void, std::string>("webfront.developer.commit");
        expose<&Store::abort, void, std::string>("webfront.developer.abort");
    }

    /// Registers a store operation as a bridge callback; the store stays alive as long as the callback.
    template <auto Member, typename Result, typename... Args>
    void expose(std::string name) {
        this->template cppFunction<Result, Args...>(std::move(name), [owned = store](Args... arguments) -> Result {
            return ((*owned).*Member)(std::move(arguments)...);
        });
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
