// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-19
//
// WebFront — frontend
// Role: Navigation, new-window and response decisions of the WebKitGTK frontend, applying the bound content policy, and the repair of an inherited Snap environment.
// Thread: ui-loop | Alloc: heap-init
// LLM: decision logic kept free of GTK calls so unit tests can reach it; scrubInheritedSnapEnvironment() mutates the process environment, so it runs on the main thread before the toolkit starts and before other threads read the environment

#pragma once

#include "../ContentPolicy.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
    #include <cerrno>
    #include <cstdlib>

    #include <fcntl.h>
    #include <poll.h>
    #include <signal.h>
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

enum class SandboxMode { development, distribution };

#if defined(__linux__)

// These variables can point the host toolkit or its children at libraries and modules
// from a different runtime. Inspect names only in diagnostics; values may be private.
inline bool containsSnapPath(std::string_view value, std::string_view separators = {}) {
    while (!value.empty()) {
        const auto end  = value.find_first_of(separators);
        const auto part = value.substr(0, end);
        if (part.starts_with("/snap/") || part.starts_with("/var/lib/snapd/snap/"))
            return true;
        if (end == std::string_view::npos)
            break;
        value.remove_prefix(end + 1);
    }
    return false;
}

struct SnapSensitiveVariable {
    const char*      name;
    std::string_view separators;
};

// Variables a Snap-packaged host (an IDE terminal) can leave pointing at its own runtime. GIO_MODULE_DIR alone is enough
// to break WebKit's network process: the Snap's GIO modules carry an RPATH to /snap/core20, which loads its libpthread.
inline constexpr std::array snapSensitiveVariables = {
    SnapSensitiveVariable{     "LD_LIBRARY_PATH", ":;"},
    SnapSensitiveVariable{          "LD_PRELOAD", ": "},
    SnapSensitiveVariable{      "GIO_MODULE_DIR",   ""},
    SnapSensitiveVariable{            "GTK_PATH",  ":"},
    SnapSensitiveVariable{      "GTK_EXE_PREFIX",   ""},
    SnapSensitiveVariable{  "GTK_IM_MODULE_FILE",   ""},
    SnapSensitiveVariable{"GSETTINGS_SCHEMA_DIR",   ""},
    SnapSensitiveVariable{             "LOCPATH",  ":"},
    SnapSensitiveVariable{       "XDG_DATA_DIRS",  ":"},
    SnapSensitiveVariable{     "XDG_CONFIG_DIRS",  ":"}
};

/// True when `name` still holds what a Snap-packaged editor put there: a Snap path, or any value that differs from the one
/// the editor saved in `<NAME>_VSCODE_SNAP_ORIG` (the editor's own mark that it overrode the variable). The second case
/// catches a per-user directory such as `~/snap/codium/common/.cache/gio-modules`, whose files link into the Snap.
inline bool snapOverridden(const SnapSensitiveVariable& variable) {
    const auto* value = std::getenv(variable.name);
    if (!value)
        return false;
    if (containsSnapPath(value, variable.separators))
        return true;
    const auto* saved = std::getenv((std::string(variable.name) + "_VSCODE_SNAP_ORIG").c_str());
    return saved && std::string_view(saved) != value;
}

inline bool inheritedSnapToolkitPaths() {
    for (const auto& variable : snapSensitiveVariables)
        if (snapOverridden(variable))
            return true;
    return false;
}

/// `value` without its Snap entries; `separators` must not be empty. Other entries are kept as they are, empty ones
/// included: an empty element of LD_LIBRARY_PATH names the current directory.
inline std::string withoutSnapPaths(std::string_view value, std::string_view separators) {
    std::string kept;
    bool        first = true, keptEmpty = false;
    for (;;) {
        const auto end  = value.find_first_of(separators);
        const auto part = value.substr(0, end);
        if (!containsSnapPath(part)) {
            if (!first)
                kept += separators.front();
            kept += part;
            first     = false;
            keptEmpty = keptEmpty || part.empty();
        }
        if (end == std::string_view::npos)
            break;
        value.remove_prefix(end + 1);
    }
    // A lone empty entry would read as "no path" once joined: spell the current directory it stands for.
    return kept.empty() && keptEmpty ? "." : kept;
}

/// The line printed on stderr when `names` were repaired, independent of the logger, which is silent until enabled.
inline std::string snapRepairMessage(const std::vector<std::string>& names) {
    std::string list;
    for (const auto& name : names)
        list += (list.empty() ? "" : ", ") + name;
    return "WebFront: inherited Snap environment would break WebKit's processes; repaired " + list
         + " for this process (see docs/WEBKITGTK.md, \"Snap-hosted editor terminals\")\n";
}

inline bool keepSnapEnvironmentRequested() {
    const auto* value = std::getenv("WEBFRONT_KEEP_SNAP_ENVIRONMENT");
    return value && std::string_view(value) != "0";
}

/// Repairs the variables that name Snap paths, for the application and the processes WebKit launches. A Snap-packaged
/// VS Code or VSCodium terminal keeps the original value in `<NAME>_VSCODE_SNAP_ORIG`: that value is the starting
/// point (an empty one means unset), and a Snap path inside it is dropped like any other, since the editor's own
/// environment may have carried one. Without a saved original the starting point is the current value. From it, drop the
/// Snap entries of a list, or unset a single Snap path. The saved original is rewritten to the repaired value, so a
/// second call finds nothing to do. Returns the names changed. The caller runs it once, on the main thread before the
/// toolkit starts and before other threads read the environment (setenv is not safe against a concurrent getenv), and
/// never inside a verified Snap confinement.
inline std::vector<std::string> scrubInheritedSnapEnvironment() {
    std::vector<std::string> changed;
    for (const auto& [name, separators] : snapSensitiveVariables) {
        if (!snapOverridden({name, separators}))
            continue;
        const std::string original = std::string(name) + "_VSCODE_SNAP_ORIG";
        const auto*       saved    = std::getenv(original.c_str());
        const std::string start    = saved ? saved : std::getenv(name);
        std::string       replacement;
        if (!separators.empty())
            replacement = withoutSnapPaths(start, separators);
        else if (!containsSnapPath(start))
            replacement = start;
        if (!replacement.empty())
            setenv(name, replacement.c_str(), 1);
        else
            unsetenv(name);
        if (saved)
            setenv(original.c_str(), replacement.c_str(), 1);
        changed.emplace_back(name);
    }
    return changed;
}

/// True for the first caller of `flag` only: the repair belongs to one early initialization phase, never to a later call
/// that could find the server and other threads already running.
inline bool claimOnce(std::atomic<bool>& flag) {
    return !flag.exchange(true);
}

// Development is the default, independently of optimization level. A distribution binary requires the sandbox;
// an inherited environment variable cannot weaken its build policy.
inline bool sandboxRequiredByEnvironment() {
    const auto* value = std::getenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX");
    return value && *value && std::string_view(value) != "0";
}

// WebKit treats the value "0" as keeping the sandbox; other present values disable it.
inline bool webKitSandboxDisabled() {
    const auto* value = std::getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    return value && std::string_view(value) != "0";
}

inline bool snapEnvironmentPresent() {
    return std::getenv("SNAP") && std::getenv("SNAP_NAME") && std::getenv("SNAP_REVISION");
}

inline bool snapConfinementVerified() {
    std::ifstream labelFile("/proc/self/attr/current");
    std::string   label;
    std::getline(labelFile, label);
    const auto* name = std::getenv("SNAP_NAME");
    return name && label.starts_with("snap." + std::string(name) + ".") && label.ends_with(" (enforce)");
}

inline bool sandboxLauncherApplies() {
    std::error_code error;
    return !webKitSandboxDisabled() && !snapEnvironmentPresent() && !std::filesystem::exists("/.flatpak-info", error);
}

inline std::filesystem::path sandboxLauncherPath() {
    #ifdef WEBFRONT_WEBKITGTK_BWRAP_EXECUTABLE
    return WEBFRONT_WEBKITGTK_BWRAP_EXECUTABLE;
    #else
    return "bwrap";  // Standalone headers use PATH; CMake consumers can pin WebKit's compiled launcher path.
    #endif
}

/// Match WebKit's container capability probe. Every failure must preserve the distribution requirement.
inline std::optional<std::string> sandboxLauncherFailure(const std::filesystem::path& launcher, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0)
        return "cannot create bubblewrap diagnostic pipe";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, report[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, report[1], STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGCHLD);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    // The shell owns/reaps the probe child. The application's SIGCHLD handler cannot steal that child's status.
    // Status travels over the pipe; waiting for the shell is only zombie cleanup, never the success criterion.
    std::string path        = launcher.string();
    const char* arguments[] = {"sh",
                               "-c",
                               "\"$@\"; result=$?; printf '\\nWEBFRONT_PROBE_STATUS=%s\\n' \"$result\"",
                               "webfront-probe",
                               path.c_str(),
                               "--ro-bind",
                               "/",
                               "/",
                               "--proc",
                               "/proc",
                               "--dev",
                               "/dev",
                               "--unshare-all",
                               "true",
                               nullptr};
    pid_t       child       = 0;
    const int   spawned     = posix_spawn(&child, "/bin/sh", &actions, &attributes, const_cast<char* const*>(arguments), ::environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(report[1]);
    if (spawned != 0) {
        close(report[0]);
        return "cannot spawn bubblewrap probe supervisor";
    }
    fcntl(report[0], F_SETFL, O_NONBLOCK);
    std::string message;
    bool        ended    = false;
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd    descriptor{report[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, 10);
        if (ready < 0 && errno != EINTR)
            break;
        char       buffer[256];
        const auto count = read(report[0], buffer, sizeof buffer);
        if (count > 0) {
            if (message.size() < 4096)
                message.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0) {
            ended = true;
            break;
        } else if (errno != EINTR && errno != EAGAIN)
            break;
    }
    close(report[0]);
    if (!ended)
        kill(-child, SIGKILL);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
    if (!ended)
        return "bubblewrap probe timed out or its diagnostic channel failed";
    constexpr std::string_view marker = "\nWEBFRONT_PROBE_STATUS=";
    const auto                 result = message.rfind(marker);
    if (result != std::string::npos && message.substr(result + marker.size()) == "0\n")
        return std::nullopt;
    if (result != std::string::npos)
        message.resize(result);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message.empty() ? "bubblewrap probe failed without a diagnostic" : message;
}

inline bool webKitContainerFallbackPossible() {
    std::error_code error;
    return std::filesystem::exists("/run/.containerenv", error);
}

inline void requireSandboxEnvironment(SandboxMode mode, bool containerFallbackPossible = webKitContainerFallbackPossible()) {
    const bool required = mode == SandboxMode::distribution || sandboxRequiredByEnvironment();
    if (required && containerFallbackPossible)
        throw std::runtime_error("WebKitGTK: distribution mode refuses containers with /run/.containerenv because WebKit may silently disable its sandbox "
                                 "after the preflight probe");
    if (required && snapEnvironmentPresent() && !snapConfinementVerified())
        throw std::runtime_error(
            "WebKitGTK: distribution mode rejects unverified Snap environment variables; launch outside Snap or use an enforced Snap profile");
    if (required && webKitSandboxDisabled())
        throw std::runtime_error("WebKitGTK: distribution mode requires the sandbox, but "
                                 "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS disables it; unset it or set it to 0");
}

/// Prepare the development policy without mutating the environment. Distribution (or the legacy REQUIRE switch)
/// rejects an inherited sandbox opt-out and reports a refused launcher before WebKit can abort the process.
/// Returns a notice only when the development policy was selected.
inline std::optional<std::string> prepareSandboxLaunch(const std::filesystem::path& launcher, SandboxMode mode) {
    if (mode == SandboxMode::development && !sandboxRequiredByEnvironment()) {
        return "development";
    }
    requireSandboxEnvironment(mode);
    if (!sandboxLauncherApplies())
        return std::nullopt;
    if (const auto refusal = sandboxLauncherFailure(launcher))
        throw std::runtime_error("WebKitGTK: the distribution sandbox cannot start (" + *refusal
                                 + "); install the application's AppArmor profile or use a configured system browser; "
                                   "see docs/WEBKITGTK.md, \"Distribution\"");
    return std::nullopt;
}

// WebKit snapshots the switch when constructing its WebContext/process pool. Restore it before any user callback.
class DevelopmentSandboxEnvironment {
    std::optional<std::string> previous;
    bool                       changed = false;

public:
    explicit DevelopmentSandboxEnvironment(SandboxMode mode) {
        if (mode != SandboxMode::development || sandboxRequiredByEnvironment())
            return;
        if (const auto* value = std::getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"))
            previous = value;
        if (setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1) != 0)
            throw std::runtime_error("WebKitGTK: cannot configure the development sandbox policy");
        changed = true;
    }
    DevelopmentSandboxEnvironment(const DevelopmentSandboxEnvironment&) = delete;
    ~DevelopmentSandboxEnvironment() {
        if (!changed)
            return;
        if (previous)
            setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", previous->c_str(), 1);
        else
            unsetenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    }
};

/// Sets environment variables for the lifetime of the object and restores them, so the application's own environment is
/// unchanged once the child processes WebKit launches in the meantime have inherited the values.
class ScopedEnvironment {
    struct Entry {
        std::string                name;
        std::optional<std::string> previous;
    };
    std::vector<Entry> entries;

public:
    ScopedEnvironment()                                    = default;
    ScopedEnvironment(const ScopedEnvironment&)            = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
    ScopedEnvironment(ScopedEnvironment&& other) noexcept : entries(std::exchange(other.entries, {})) { }
    void set(const char* name, const std::string& value) {
        Entry entry{name, std::nullopt};
        if (const auto* current = std::getenv(name))
            entry.previous = current;
        if (setenv(name, value.c_str(), 1) != 0)
            throw std::runtime_error(std::string("WebKitGTK: cannot set ") + name);
        entries.push_back(std::move(entry));
    }
    ~ScopedEnvironment() {
        for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
            if (entry->previous)
                setenv(entry->name.c_str(), entry->previous->c_str(), 1);
            else
                unsetenv(entry->name.c_str());
        }
    }
};

#endif

}  // namespace webfront::frontend::detail::webkitgtk

namespace webfront::frontend {
struct WebKitGTKHost { };
template <detail::webkitgtk::SandboxMode Mode>
struct BasicWebKitGTKFrontend;
#if defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION
using WebKitGTKFrontend = BasicWebKitGTKFrontend<detail::webkitgtk::SandboxMode::distribution>;
#else
using WebKitGTKFrontend = BasicWebKitGTKFrontend<detail::webkitgtk::SandboxMode::development>;
#endif
}  // namespace webfront::frontend
