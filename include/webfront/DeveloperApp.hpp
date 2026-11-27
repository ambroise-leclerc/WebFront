// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-20
//
// WebFront — core
// Role: Opt-in developer environment (terminal and wf commands) running in the application window.
// Thread: ui-loop | Alloc: heap-init
// LLM: public API; downloaded code has project and bridge authority, read DeveloperBootstrapOptions first

#pragma once

#include "BridgeState.hpp"
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

/// The identifier of the page that holds the tab (the inspectors' host): 1 to 64 letters, digits, `-` or `_`. It names the
/// host in the page of a detached panel, so that the panel is attached again to the page that detached it only.
inline bool inspectorHostIdentifier(std::string_view host) noexcept {
    return !host.empty() && host.size() <= 64 && std::ranges::all_of(host, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

/// Where the inspectors keep the detached panels and the floating window's place, in the project (inspector-host.mjs).
inline constexpr std::string_view inspectorPreferencesPath = ".webfront/developer/inspector-window.json";

/// The page of the environment that shows an inspector panel for a host page, as openSecondaryWindow() names its window:
/// one window per panel and host, so that the same host opening a panel again brings its window to the front. Throws
/// std::invalid_argument for an unknown panel or an invalid host identifier.
inline std::string inspectorPage(std::string_view panel, std::string_view host) {
    if (std::ranges::find(inspectorPanels, panel) == inspectorPanels.end())
        throw std::invalid_argument("Unknown inspector panel");
    if (!inspectorHostIdentifier(host))
        throw std::invalid_argument("Invalid inspector host identifier");
    return "webfront/developer/environment/inspector.html?panel=" + std::string(panel) + "&host=" + std::string(host);
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
        exposeWrite<&Store::beginReplacing, std::string, std::string, bool>("webfront.developer.begin");
        exposeWrite<&Store::beginNew, std::string, std::string, bool>("webfront.developer.beginNew");
        exposeWrite<&Store::write, void, std::string, std::string, std::uint64_t, Bytes>("webfront.developer.write");
        exposeWrite<&Store::commit, void, std::string>("webfront.developer.commit");
        exposeWrite<&Store::abort, void, std::string>("webfront.developer.abort");
        expose<&Store::stateOfLines, std::string, std::string>("webfront.developer.state");
        exposeWrite<&Store::commitGroupLines, void, std::string, std::string, std::string>("webfront.developer.commitGroup");
#if !(defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION)
        registerInspectors();
#endif
    }

#if !(defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION)
    /// The inspectors exist only in the developer environment and never in a distribution build (ADR-027, section 1). Every
    /// function but containers() answers only an admitted page (UI::isLocalClient, section 2).
    void registerInspectors() {
        using Policy = http::DefaultBuffersPolicy;
        // A policy too small for a page of one node, or for a write of 1 KiB, does not compile (ADR-027, section 3).
        static constexpr inspect::Limits limits = inspect::limitsOf<Policy>();
        using Caller                            = typename Base::UI;
        const auto registry                     = this->enableInspectors();
        // The menu of the environment's tab asks it first: an environment that does not find it says the library is too old.
        this->template reservedCppFunction<std::tuple<bool, bool, bool>, Caller>(
            "webfront.inspector.containers",
            [trust = this->trustsLoopbackClients()](Caller ui) {
                return developer::inspectorContainers(Base::canShowSecondaryWindow(), ui.isLocalClient(), trust);
            });
        // Whether the calling page may run in an embedded frontend: a tab of the system browser is offered only to a page
        // that cannot (ADR-027, section 8), since window.open() in an embedded view would open a frame without the proof.
        // The proof tells a proven view, not where an unproven page runs: while the active frontend gives its views the
        // proof, an admitted page without it (a frame of such a view, under trustLoopbackClients) may be inside one too.
        this->template reservedCppFunction<bool, Caller>("webfront.inspector.embedded", [](Caller ui) {
            admit(ui);
            return ui.isEmbeddedView() || frontend::providesViewProof<Frontend>();
        });
        // The inspectors' choices, read with the token of exactly the bytes returned, then written only if the file still has
        // that content: a page applies its changes to what it read, never to a content that changed and came back meanwhile.
        this->template reservedCppFunction<std::tuple<bool, std::vector<std::uint8_t>, std::string>, Caller>(
            "webfront.inspector.readPreferences",
            [owned = store](Caller ui) {
                admit(ui);
                return owned->readWithState(std::string{developer::inspectorPreferencesPath}, 64 * 1024);
            });
        this->template reservedCppFunction<void, Caller, std::string, std::vector<std::uint8_t>>(
            "webfront.inspector.savePreferences",
            [owned = store](Caller ui, std::string expected, std::vector<std::uint8_t> bytes) {
                admit(ui);
                owned->publishFile(std::string{developer::inspectorPreferencesPath}, std::move(bytes), expected);
            });
        // The discovery of the watches (section 3): a read of the table, without calling any source.
        this->template reservedCppFunction<inspect::WatchPage, Caller, double, double, double>(
            "webfront.inspector.watches",
            [registry](Caller ui, double revision, double after, double count) {
                admit(ui);
                return registry->page(revision, after, count, inspect::responseBudget<Policy>);
            });
        // The watches' trees (sections 3, 5 and 6). Each operation runs whole where the watch's object lives: on its executor
        // when it has one, else here, on the server thread. The session is taken while the caller is known to be connected.
        this->template reservedCppFunction<inspect::LimitsAnswer, Caller>("webfront.inspector.limits", [](Caller ui) {
            admit(ui);
            return inspect::WatchRegistry::limits(limits);
        });
        this->template reservedCppFunction<ResultTask<inspect::Page>, Caller, double, std::string, double, double, double>(
            "webfront.inspector.children",
            [this, registry](Caller ui, double watch, std::string path, double revision, double start, double count) {
                admit(ui);
                return inspect::onWatch<inspect::Page>(registry, watch, [=, session = this->inspectorSession(ui)] {
                    return registry->children(session, watch, path, revision, start, count, limits);
                });
            });
        this->template reservedCppFunction<ResultTask<inspect::Page>, Caller, double, double, double, double>(
            "webfront.inspector.changes",
            [this, registry](Caller ui, double watch, double revision, double cursor, double count) {
                admit(ui);
                return inspect::onWatch<inspect::Page>(registry, watch, [=, session = this->inspectorSession(ui)] {
                    return registry->changes(session, watch, revision, cursor, count, limits);
                });
            });
        this->template reservedCppFunction<ResultTask<inspect::ValueAnswer>, Caller, double, std::string, double, double>(
            "webfront.inspector.value",
            [this, registry](Caller ui, double watch, std::string path, double snapshot, double offset) {
                admit(ui);
                return inspect::onWatch<inspect::ValueAnswer>(registry, watch, [=, session = this->inspectorSession(ui)] {
                    return registry->value(session, watch, path, snapshot, offset, limits);
                });
            });
        this->template reservedCppFunction<ResultTask<inspect::Page>, Caller, double, std::string, double>(
            "webfront.inspector.fold",
            [this, registry](Caller ui, double watch, std::string path, double revision) {
                admit(ui);
                return inspect::onWatch<inspect::Page>(registry, watch, [=, session = this->inspectorSession(ui)] {
                    return registry->fold(session, watch, path, revision);
                });
            });
        this->template reservedCppFunction<ResultTask<double>, Caller, double>("webfront.inspector.reset", [this, registry](Caller ui, double watch) {
            admit(ui);
            return inspect::onWatch<double>(registry, watch, [=, session = this->inspectorSession(ui)] {
                return registry->reset(session, watch);
            });
        });
        this->template reservedCppFunction<ResultTask<inspect::WriteAnswer>, Caller, double, double, std::string, std::string, std::string>(
            "webfront.inspector.write",
            [this, registry](Caller ui, double watch, double revision, std::string path, std::string expected, std::string text) {
                admit(ui);
                return inspect::onWatch<inspect::WriteAnswer>(registry, watch, [=, session = this->inspectorSession(ui)] {
                    return registry->write(session, watch, revision, path, expected, text, limits);
                });
            });
        // What the application registered and runs (section 7): pages of rows, each within the answer's budget.
        this->template reservedCppFunction<inspect::FunctionPage, Caller, double, double>(
            "webfront.inspector.functions",
            [functions = this->inspectedFunctions()](Caller ui, double after, double count) {
                admit(ui);
                return functions->page(after, count, inspect::responseBudget<Policy>);
            });
        this->template reservedCppFunction<inspect::JsFunctionPage, Caller, double, double>(
            "webfront.inspector.jsFunctions",
            [calls = this->inspectedJsCalls()](Caller ui, double after, double count) {
                admit(ui);
                return calls->page(after, count, inspect::responseBudget<Policy>);
            });
        this->template reservedCppFunction<inspect::BrowserPage, Caller, double, double>(
            "webfront.inspector.browsers",
            [this](Caller ui, double from, double count) {
                admit(ui);
                return inspect::browserPage(this->inspectedBrowsers(ui), from, count, this->inspectedTasks(), inspect::responseBudget<Policy>);
            });
        // The state of the bridge in a page (#684, section 8): a detached panel is another window, which does not see the page
        // of the application, so that page publishes its state here and the panel reads it, both under the inspectors' access.
        // A page publishes only while a panel reads: bridgeSource() announces it and says whether one does, a read asks an
        // idle page to start (wfInspect.publish, inspector-bridge.mjs), and each publication is told whether to go on.
        const auto bridges = std::make_shared<inspect::BridgeStates>();
        this->template reservedCppFunction<bool, Caller, std::string>("webfront.inspector.bridgeSource", [bridges](Caller ui, std::string host) {
            admit(ui);
            requireHost(host);
            return bridges->announce(
                host,
                ui,
                [ui] {
                    ui.jsFunction("wfInspect.publish")();
                },
                [ui] {
                    return ui.isLocalClient();
                });
        });
        this->template reservedCppFunction<bool, Caller, std::string, std::vector<inspect::BridgeRow>>(
            "webfront.inspector.publishBridge",
            [bridges](Caller ui, std::string host, std::vector<inspect::BridgeRow> rows) {
                admit(ui);
                requireHost(host);
                return bridges->publish(host, ui, std::move(rows));
            });
        this->template reservedCppFunction<inspect::BridgePage, Caller, std::string, double, double>(
            "webfront.inspector.bridge",
            [bridges](Caller ui, std::string host, double from, double count) {
                admit(ui);
                requireHost(host);
                auto read = bridges->read(host, from, count, inspect::responseBudget<Policy>);
                if (read.ask)
                    read.ask();
                return std::move(read.page);
            });
        // A secondary window of the panel for the host page, with the proof of the view (section 8); opening it again brings it to the front.
        // The window is opened on the interface thread, where openSecondaryWindow() does not hold the server's thread.
        this->template reservedCppFunction<ResultTask<void>, Caller, std::string, std::string, double, double>(
            "webfront.inspector.openWindow",
            [this](Caller ui, std::string panel, std::string host, double width, double height) -> ResultTask<void> {
                admit(ui);
                auto page = developer::inspectorPage(panel, host);
                if (!Base::canShowSecondaryWindow())
                    throw webfront::detail::makeBridgeError(inspect::code::unavailable, "This frontend cannot open a secondary window");
                WindowOptions options{.title  = "WebFront inspector",
                                      .width  = developer::inspectorWindowExtent(width, 720),
                                      .height = developer::inspectorWindowExtent(height, 560)};
                // Bound to the interface session that runs when the request is admitted: if that loop ends before the work
                // reaches the interface thread, a loop started since is not given a window it was not asked for.
                const auto bound = this->currentShowSession();
                // The continuation of then() runs on the interface thread; the task itself resumes on the server's.
                co_await networking::ex::then(this->uiExecutor().schedule(), [&] {
                    if (!this->isCurrentShowSession(bound))
                        throw webfront::detail::makeBridgeError(inspect::code::unavailable, "The interface session has ended");
                    this->openSecondaryWindow(page, std::move(options));
                });
            });
        // Closes the panel's secondary window of that host, when it is attached again; false when none is open.
        this->template reservedCppFunction<ResultTask<bool>, Caller, std::string, std::string>(
            "webfront.inspector.closeWindow",
            [this](Caller ui, std::string panel, std::string host) -> ResultTask<bool> {
                admit(ui);
                auto page = developer::inspectorPage(panel, host);
                if (!Base::canShowSecondaryWindow())
                    co_return false;
                const auto bound = this->currentShowSession();  // as openWindow: never a window of a later session
                co_return co_await networking::ex::then(this->uiExecutor().schedule(), [&] {
                    return this->isCurrentShowSession(bound) && this->requestClose(page);
                });
            });
    }

#endif

    /// Refuses an identifier that does not name a host page (developer::inspectorHostIdentifier).
    static void requireHost(std::string_view host) {
        if (!developer::inspectorHostIdentifier(host))
            throw std::invalid_argument("Invalid inspector host identifier");
    }

    /// Refuses a page that is neither the proven view nor, with trustLoopbackClients, a loopback page (ADR-027, section 2).
    static void admit(const typename Base::UI& ui) {
        if (!ui.isLocalClient())
            throw webfront::detail::makeBridgeError(inspect::code::denied,
                                                    "The inspectors answer only a view of the application (or a loopback page with trustLoopbackClients)");
    }

    /// Registers a store operation as a bridge callback; the store stays alive as long as the callback.
    template <auto Member, typename Result, typename... Args>
    void expose(std::string name) {
        this->template cppFunction<Result, Args...>(std::move(name), [owned = store](Args... arguments) -> Result {
            return ((*owned).*Member)(std::move(arguments)...);
        });
    }

    /// Writes are restricted when the active frontend proves the application's view (ADR-027, section 2).
    template <auto Member, typename Result, typename... Args>
    void exposeWrite(std::string name) {
        using Caller = typename Base::UI;
        this->template cppFunction<Result, Caller, Args...>(std::move(name), [owned = store](Caller ui, Args... arguments) -> Result {
            // The inspectors' code (ADR-027, section 2), with a message that names what was refused.
            if (frontend::providesViewProof<Frontend>() && !ui.isLocalClient())
                throw webfront::detail::makeBridgeError(inspect::code::denied,
                                                        "Project writes require an admitted application view (or a loopback page with trustLoopbackClients)");
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
