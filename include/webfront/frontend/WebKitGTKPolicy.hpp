#pragma once

#include "../ContentPolicy.hpp"

#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__linux__)
    #include <cerrno>
    #include <cstdlib>

    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace webfront::frontend::detail::webkitgtk {

// WebKitPolicyDecisionType values from the stable C API. No GTK objects are needed here.
enum class PolicyEvent { navigation, newWindow, response };
enum class PolicyDecision { allow, cancel, popup, failInitialNavigation };

struct PolicyRequest {
    PolicyEvent event;
    const char* uri               = nullptr;
    bool        mainResource      = false;
    unsigned    status            = 0;
    bool        initialNavigation = false;
};

inline bool navigationSchemeAllowed(const char* uri) {
    if (!uri)
        return false;
    if (webfront::detail::contentOrigin(uri) || webfront::detail::contentOrigin(webfront::detail::blobOriginUrl(uri)))
        return true;
    return std::string_view(uri) == "about:blank" || std::string_view(uri) == "about:srcdoc";
}

inline PolicyDecision decideResponse(const webfront::detail::BoundContentPolicy& policy, const PolicyRequest& request) {
    if (!request.mainResource)
        return PolicyDecision::allow;
    if (!request.uri || !policy.allowsNavigation(request.uri))
        return PolicyDecision::cancel;
    if (request.initialNavigation && request.status >= 400)
        return PolicyDecision::failInitialNavigation;
    return PolicyDecision::allow;
}

inline PolicyDecision decidePolicy(const webfront::detail::BoundContentPolicy& policy, const PolicyRequest& request) {
    switch (request.event) {
        case PolicyEvent::response:
            return decideResponse(policy, request);
        case PolicyEvent::newWindow:
            return PolicyDecision::popup;
        case PolicyEvent::navigation:
            // Unnamed iframes cannot be identified here; reject opaque schemes in every frame.
            return navigationSchemeAllowed(request.uri) ? PolicyDecision::allow : PolicyDecision::cancel;
    }
    return PolicyDecision::cancel;
}

inline std::string initialNavigationError(const char* message) {
    return std::string("initial navigation failed: ") + (message ? message : "unknown error") + "; check the local server and page path";
}

struct FailureState {
    std::exception_ptr failure;

    void rememberFailure() noexcept {
        if (!failure)
            failure = std::current_exception();
    }
    void rethrowIfFailed() const {
        if (failure)
            std::rethrow_exception(failure);
    }
    void fail(std::string_view message) noexcept {
        if (failure)
            return;
        try {
            throw std::runtime_error("WebKitGTK: " + std::string(message));
        } catch (...) {
            rememberFailure();
        }
    }
};

#if defined(__linux__)

// Development is the default, independently of optimization level. A distribution binary requires the sandbox;
// an inherited environment variable cannot weaken its build policy.
enum class SandboxMode { development, distribution };

inline constexpr SandboxMode buildSandboxMode =
    #if defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION
    SandboxMode::distribution;
    #else
    SandboxMode::development;
    #endif

inline bool sandboxRequiredByEnvironment() {
    const auto* value = std::getenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX");
    return value && *value && std::string_view(value) != "0";
}

// WebKit treats the value "0" as keeping the sandbox; other present values disable it.
inline bool webKitSandboxDisabled() {
    const auto* value = std::getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    return value && std::string_view(value) != "0";
}

/// Whether WebKit would use bubblewrap here: not with its sandbox switched off, nor inside Flatpak, Snap or a container.
inline bool sandboxLauncherApplies() {
    const auto set = [](const char* name) {
        const auto* value = std::getenv(name);
        return value && *value;
    };
    const auto exists = [](const char* path) {
        std::error_code error;
        return std::filesystem::exists(path, error);
    };
    if (webKitSandboxDisabled())
        return false;
    if (set("SNAP") && set("SNAP_NAME") && set("SNAP_REVISION"))
        return false;
    return !exists("/.flatpak-info") && !exists("/.dockerenv") && !exists("/run/.containerenv");
}

/// Runs `launcher --ro-bind / / launcher --version`. Returns the launcher's own message when it could not set up its
/// user namespace, nullopt when it could or when nothing can be concluded (no launcher, another kind of failure).
inline std::optional<std::string> sandboxLauncherRefusal(const std::filesystem::path& launcher) {
    if (access(launcher.c_str(), X_OK) != 0)
        return std::nullopt;
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0)
        return std::nullopt;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, report[1], STDERR_FILENO);
    const std::string path   = launcher.string();
    char              bind[] = "--ro-bind", root[] = "/", version[] = "--version";
    std::string       program = path, inner = path;
    char* const       arguments[] = {program.data(), bind, root, root, inner.data(), version, nullptr};
    pid_t             child       = 0;
    const int         spawned     = posix_spawn(&child, path.c_str(), &actions, nullptr, arguments, ::environ);
    posix_spawn_file_actions_destroy(&actions);
    close(report[1]);
    std::string message;
    if (spawned == 0) {
        char buffer[256];
        for (;;) {
            const auto count = read(report[0], buffer, sizeof buffer);
            if (count > 0 && message.size() < 1024)
                message.append(buffer, static_cast<std::size_t>(count));
            else if (count == 0 || (count < 0 && errno != EINTR))
                break;
        }
    }
    close(report[0]);
    if (spawned != 0)
        return std::nullopt;
    int   status = 0;
    pid_t waited;
    do
        waited = waitpid(child, &status, 0);
    while (waited < 0 && errno == EINTR);
    if (waited != child || (WIFEXITED(status) && WEXITSTATUS(status) == 0))
        return std::nullopt;
    // bubblewrap's wording for a refused namespace; anything else is left for WebKit to report.
    if (message.find("uid map") == std::string::npos && message.find("namespace") == std::string::npos)
        return std::nullopt;
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message;
}

/// Development explicitly disables WebKit's sandbox. Distribution (or the legacy REQUIRE environment switch)
/// rejects an inherited sandbox opt-out and reports a refused launcher before WebKit can abort the process.
/// Returns a notice only when the development policy was selected.
inline std::optional<std::string> prepareSandboxLaunch(const std::filesystem::path& launcher = "/usr/bin/bwrap", SandboxMode mode = buildSandboxMode) {
    if (mode == SandboxMode::development && !sandboxRequiredByEnvironment()) {
        if (setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1) != 0)
            throw std::runtime_error("WebKitGTK: cannot configure the development sandbox policy");
        return "development";
    }
    if (webKitSandboxDisabled())
        throw std::runtime_error("WebKitGTK: distribution mode requires the sandbox, but "
                                 "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS disables it; unset it or set it to 0");
    if (!sandboxLauncherApplies())
        return std::nullopt;
    if (const auto refusal = sandboxLauncherRefusal(launcher))
        throw std::runtime_error("WebKitGTK: the distribution sandbox cannot start (" + *refusal
                                 + "); install the application's AppArmor profile or use a configured system browser; "
                                   "see docs/WEBKITGTK.md, \"Distribution\"");
    return std::nullopt;
}

#endif

}  // namespace webfront::frontend::detail::webkitgtk
