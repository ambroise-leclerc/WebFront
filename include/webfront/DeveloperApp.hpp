// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-20
//
// WebFront — core
// Role: Opt-in developer environment (terminal and wf commands) running in the application window.
// Thread: ui-loop | Alloc: heap-init
// LLM: public API; downloaded code has project and bridge authority, read DeveloperBootstrapOptions first

#pragma once

#include "system/DeveloperFS.hpp"
#include "system/DeveloperReleaseData.hpp"
#include "system/SafePath.hpp"
#include "WebFront.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <tuple>

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
    /// Directory of webfront.project.json and of its lock, relative to the project directory ('/'-separated, no ".."):
    /// empty for the project directory itself. The paths the manifest contains stay relative to the project directory.
    /// Needs an environment that reads "manifestDirectory" (0.6.2 or later); an earlier one looks in the project directory.
    std::string manifestDirectory{};
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
    if (!options.manifestDirectory.empty()) {
        // .webfront/ belongs to wf (cache, packages, preview), which replaces whole directories there.
        if (*fs::safeRelativePath(options.manifestDirectory).begin() == ".webfront")
            throw std::invalid_argument("The manifest directory cannot be inside .webfront");
    }
}

/// The environment contract this C++ implements. An environment's environment.json declares the "protocol" it
/// speaks and the "api" (the webfront.developer.* callbacks) it expects; the loader refuses any other version.
/// Changing a callback increments apiVersion and requires publishing a new bootstrap environment.
inline constexpr int protocolVersion = 1;
inline constexpr int apiVersion      = 1;

inline constexpr std::string_view defaultRegistry{"https://registry.npmjs.org/"};

/// What webfront.inspector.containers() answers (ADR-027, section 8): whether the calling page can open a secondary window
/// that reaches the inspectors (the frontend opens one and the page is admitted, since opening it is checked like any
/// inspector call), whether the page itself is admitted (UI::isLocalClient), and whether a new connection that is not
/// proven would be (trustLoopbackClients). Three booleans and no state of the application: the call checks no access.
inline constexpr std::tuple<bool, bool, bool> inspectorContainers(bool frontendOpensWindows, bool hostAdmitted, bool trustLoopback) noexcept {
    return {frontendOpensWindows && hostAdmitted, hostAdmitted, trustLoopback};
}

/// The inspector panels a detached window can show (the entries of the tab's menu, js/developer/environment/menu.mjs).
inline constexpr std::array<std::string_view, 5> inspectorPanels{"cpp.functions", "cpp.browsers", "cpp.watches", "typescript.packages", "typescript.bridge"};

/// The page of the environment that shows an inspector panel, as openSecondaryWindow() names its window: one window per
/// panel, so that opening a panel again brings its window to the front. Throws std::invalid_argument for an unknown panel.
inline std::string inspectorPage(std::string_view panel) {
    if (std::ranges::find(inspectorPanels, panel) == inspectorPanels.end())
        throw std::invalid_argument("Unknown inspector panel");
    return "webfront/developer/environment/inspector.html?panel=" + std::string(panel);
}

/// A window size asked by the page, as a whole number of pixels within what a screen can show.
inline int inspectorWindowExtent(double value, int fallback) {
    if (!std::isfinite(value) || value < 320 || value > 8192)
        return fallback;
    return static_cast<int>(value);
}

/// The configuration the environment reads. "registry" always names a usable registry, for environments
/// that predate "registryExplicit"; the latter tells whether the application chose it.
inline std::string bootstrapConfig(const DeveloperBootstrapOptions& options) {
    return "{\"environment\":" + json::quote(options.environment) + ",\"integrity\":" + json::quote(options.integrity) + ",\"registry\":"
           + json::quote(options.registry.value_or(std::string(defaultRegistry))) + ",\"registryExplicit\":" + (options.registry ? "true" : "false")
           + (options.manifestDirectory.empty() ? std::string{} : ",\"manifestDirectory\":" + json::quote(options.manifestDirectory))
           + ",\"supported\":{\"protocol\":[" + std::to_string(protocolVersion) + "],\"api\":[" + std::to_string(apiVersion) + "]}}";
}
}  // namespace developer

template <typename Network, frontend::FrontendType Frontend, fs::Provider... Extra>
class DeveloperAppWithNetwork : public BasicWF<Network, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend> {
    using Base = BasicWF<Network, fs::Multi<fs::DeveloperFS, Extra...>, http::DefaultBuffersPolicy, Frontend>;

public:
    /// An explicit project is required before enabling development file operations.
    explicit DeveloperAppWithNetwork(std::filesystem::path projectDirectory = {}, WebFrontConfig config = {})
        : Base(developer::projectConfig(std::move(config), projectDirectory)),
          project(projectDirectory.empty() ? std::filesystem::path{} : developer::canonicalDirectory(projectDirectory)) { }

    void bootstrap(std::string_view          mode,
                   DeveloperBootstrapOptions options = {.environment = std::string(developer::release::environment),
                                                        .integrity   = std::string(developer::release::integrity)}) {
        if (store)
            throw std::logic_error("Developer bootstrap is already configured");
        developer::validateBootstrap(mode, project, options);
        store = std::make_shared<developer::Store>(project);
        this->useDeveloperEnvironment();
        registerBridge(developer::bootstrapConfig(options));
    }

    /// The canonical project directory, empty when none was given.
    const std::filesystem::path& projectDirectory() const {
        return project;
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
        // Added after API 1 was published: an environment looks for it and does without (ls -l) when it is missing.
        expose<&Store::entries, Bytes, std::string>("webfront.developer.entries");
        // For the bootstrap shipped with this C++: it republishes the environment only when a published file differs.
        expose<&Store::digests, Bytes, std::string>("webfront.developer.digests");
        expose<&Store::beginReplacing, std::string, std::string, bool>("webfront.developer.begin");
        expose<&Store::beginNew, std::string, std::string, bool>("webfront.developer.beginNew");
        expose<&Store::write, void, std::string, std::string, std::uint64_t, Bytes>("webfront.developer.write");
        expose<&Store::commit, void, std::string>("webfront.developer.commit");
        expose<&Store::abort, void, std::string>("webfront.developer.abort");
        expose<&Store::stateOfLines, std::string, std::string>("webfront.developer.state");
        expose<&Store::commitGroupLines, void, std::string, std::string, std::string>("webfront.developer.commitGroup");
#if !(defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION)
        registerInspectors();
#endif
    }

#if !(defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION)
    /// The inspectors exist only in the developer environment and never in a distribution build (ADR-027, section 1). Every
    /// function but containers() answers only an admitted page (UI::isLocalClient, section 2).
    void registerInspectors() {
        using Policy = http::DefaultBuffersPolicy;
        static_assert(inspect::responseBudget<Policy> >= inspect::minPageBytes,
                      "The buffers policy is too small for the inspectors: an answer must hold a page of one node (ADR-027)");
        using Caller        = typename Base::UI;
        const auto registry = this->enableInspectors();
        // The menu of the environment's tab asks it first: an environment that does not find it says the library is too old.
        this->template reservedCppFunction<std::tuple<bool, bool, bool>, Caller>(
            "webfront.inspector.containers",
            [trust = this->trustsLoopbackClients()](Caller ui) {
                return developer::inspectorContainers(Base::canShowSecondaryWindow(), ui.isLocalClient(), trust);
            });
        // Whether the calling page is the view of an embedded frontend: a tab of the system browser is offered only to a page
        // that is not (ADR-027, section 8), since window.open() in an embedded view would open a frame without the proof.
        this->template reservedCppFunction<bool, Caller>("webfront.inspector.embedded", [](Caller ui) {
            admit(ui);
            return ui.isEmbeddedView();
        });
        // The discovery of the watches (section 3): a read of the table, without calling any source.
        this->template reservedCppFunction<inspect::WatchPage, Caller, double, double, double>(
            "webfront.inspector.watches",
            [registry](Caller ui, double revision, double after, double count) {
                admit(ui);
                return registry->page(revision, after, count, inspect::responseBudget<Policy>);
            });
        // A secondary window of the panel, with the proof of the view (section 8); opening it again brings it to the front.
        // The window is opened on the interface thread, where openSecondaryWindow() does not hold the server's thread.
        this->template reservedCppFunction<ResultTask<void>, Caller, std::string, double, double>(
            "webfront.inspector.openWindow",
            [this](Caller ui, std::string panel, double width, double height) -> ResultTask<void> {
                admit(ui);
                auto page = developer::inspectorPage(panel);
                if (!Base::canShowSecondaryWindow())
                    throw webfront::detail::makeBridgeError(inspect::code::unavailable, "This frontend cannot open a secondary window");
                WindowOptions options{.title  = "WebFront inspector",
                                      .width  = developer::inspectorWindowExtent(width, 720),
                                      .height = developer::inspectorWindowExtent(height, 560)};
                // The continuation of then() runs on the interface thread; the task itself resumes on the server's.
                co_await networking::ex::then(this->uiExecutor().schedule(), [&] {
                    this->openSecondaryWindow(page, std::move(options));
                });
            });
        // Closes the panel's secondary window, when it is attached again; false when none is open.
        this->template reservedCppFunction<ResultTask<bool>, Caller, std::string>(
            "webfront.inspector.closeWindow",
            [this](Caller ui, std::string panel) -> ResultTask<bool> {
                admit(ui);
                auto page = developer::inspectorPage(panel);
                if (!Base::canShowSecondaryWindow())
                    co_return false;
                co_return co_await networking::ex::then(this->uiExecutor().schedule(), [&] {
                    return this->requestClose(page);
                });
            });
    }

    /// Refuses a page that is neither the proven view nor, with trustLoopbackClients, a loopback page (ADR-027, section 2).
    static void admit(const typename Base::UI& ui) {
        if (!ui.isLocalClient())
            throw webfront::detail::makeBridgeError(inspect::code::denied,
                                                    "The inspectors answer only a view of the application (or a loopback page with trustLoopbackClients)");
    }
#endif

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

template <frontend::FrontendType Frontend = frontend::DefaultFrontend, fs::Provider... Extra>
using BasicDeveloperApp = DeveloperAppWithNetwork<NetProvider, Frontend, Extra...>;

#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
using NativeApp = BasicDeveloperApp<frontend::NativeWebViewFrontend>;

/// Distribution form: generated resources, protected bridge and fallback; no developer file API.
template <fs::Provider... Assets>
using NativeAppWithAssets = BasicWF<NetProvider, fs::DefaultFSWith<Assets...>, http::DefaultBuffersPolicy, frontend::NativeWebViewFrontend>;
#endif

}  // namespace webfront
