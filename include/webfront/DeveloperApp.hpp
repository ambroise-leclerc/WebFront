/// @brief Opt-in browser-powered developer environment. Downloaded code has project/bridge authority.
#pragma once

#include "system/DeveloperFS.hpp"
#include "system/DeveloperReleaseData.hpp"
#include "WebFront.hpp"

#include <optional>

namespace webfront {

struct DeveloperBootstrapOptions {
    /// HTTPS npm tarball URL, or same-origin URL for local provisioning/tests.
    std::string environment;
    /// Trusted SHA-512 SRI of that exact tarball, supplied independently of its download.
    std::string integrity;
    /// npm registry chosen by the application: HTTPS, or a same-origin path such as "/registry/" for a
    /// local mirror. It takes precedence over the project manifest's "registry"; unset, the manifest's
    /// registry applies, else registry.npmjs.org.
    std::optional<std::string> registry{};
};

namespace developer {
/// The project is the document root of the protected bridge. Its callbacks write the project, so a
/// WebSocket without an Origin header (not a browser page) is refused.
inline WebFrontConfig projectConfig(WebFrontConfig config, const std::filesystem::path& projectDirectory) {
    config.documentRoot                     = projectDirectory;
    config.contentPolicy.allowMissingOrigin = false;
    return config;
}

/// A path that stays on the application's origin: browsers read "//host" and "/\\host" as another host.
inline bool sameOriginPath(std::string_view path) {
    return path.starts_with('/') && !path.starts_with("//") && !path.contains('\\');
}

inline void validateBootstrap(std::string_view mode, const std::filesystem::path& project, const DeveloperBootstrapOptions& options) {
    if (mode != "developer")
        throw std::invalid_argument("Unknown bootstrap mode");
    if (project.empty())
        throw std::invalid_argument("Developer bootstrap requires an explicit project directory");
    if (options.environment.empty() || !options.integrity.starts_with("sha512-"))
        throw std::invalid_argument("Developer bootstrap requires an environment URL and trusted SHA-512 integrity");
    if (options.registry && !options.registry->starts_with("https://") && !developer::sameOriginPath(*options.registry))
        throw std::invalid_argument("The developer registry must be an HTTPS URL or a same-origin path");
}

/// The environment contract this C++ implements. An environment's environment.json declares the "protocol" it
/// speaks and the "api" (the webfront.developer.* callbacks) it expects; the loader refuses any other version.
/// Changing a callback increments apiVersion and requires publishing a new bootstrap environment.
inline constexpr int protocolVersion = 1;
inline constexpr int apiVersion      = 1;

inline constexpr std::string_view defaultRegistry{"https://registry.npmjs.org/"};

/// The configuration the environment reads. "registry" always names a usable registry, for environments
/// that predate "registryExplicit"; the latter tells whether the application chose it.
inline std::string bootstrapConfig(const DeveloperBootstrapOptions& options) {
    return "{\"environment\":" + json::quote(options.environment) + ",\"integrity\":" + json::quote(options.integrity)
         + ",\"registry\":" + json::quote(options.registry.value_or(std::string(defaultRegistry)))
         + ",\"registryExplicit\":" + (options.registry ? "true" : "false")
         + ",\"supported\":{\"protocol\":[" + std::to_string(protocolVersion) + "],\"api\":[" + std::to_string(apiVersion) + "]}}";
}
}  // namespace developer

template <frontend::FrontendType Frontend = frontend::DefaultFrontend, fs::Provider... Extra>
class BasicDeveloperApp : public BasicWF<NetProvider, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend> {
    using Base = BasicWF<NetProvider, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend>;

public:
    /// An explicit project is required before enabling development file operations.
    explicit BasicDeveloperApp(std::filesystem::path projectDirectory = {}, WebFrontConfig config = {})
        : Base(developer::projectConfig(std::move(config), projectDirectory)),
          project(projectDirectory.empty() ? std::filesystem::path{} : developer::canonicalDirectory(projectDirectory)) { }

    void bootstrap(std::string_view          mode,
                   DeveloperBootstrapOptions options = {.environment = std::string(developer::release::environment),
                                                        .integrity   = std::string(developer::release::integrity)}) {
        if (store)
            throw std::logic_error("Developer bootstrap is already configured");
        developer::validateBootstrap(mode, project, options);
        store = std::make_shared<developer::Store>(project);
        registerBridge(developer::bootstrapConfig(options));
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
        expose<&Store::list, Bytes, std::string>("webfront.developer.list");
        expose<&Store::beginReplacing, std::string, std::string, bool>("webfront.developer.begin");
        expose<&Store::beginNew, std::string, std::string, bool>("webfront.developer.beginNew");
        expose<&Store::write, void, std::string, std::string, std::uint64_t, Bytes>("webfront.developer.write");
        expose<&Store::commit, void, std::string>("webfront.developer.commit");
        expose<&Store::abort, void, std::string>("webfront.developer.abort");
        expose<&Store::stateOfLines, std::string, std::string>("webfront.developer.state");
        expose<&Store::commitGroupLines, void, std::string, std::string, std::string>("webfront.developer.commitGroup");
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
