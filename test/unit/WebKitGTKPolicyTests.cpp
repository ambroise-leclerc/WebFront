// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-19
//
// WebFront — tests/unit
// Role: The navigation, popup and response decisions of the WebKitGTK frontend, and its sandbox environment.
// Thread: ui-loop | Alloc: heap-init
// LLM: logic only, no GTK needed

#include "TemporaryDirectory.hpp"
#include <webfront/frontend/WebKitGTKPolicy.hpp>
#include <webfront/testing/Bdd.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
    #include <unistd.h>
#endif

using namespace webfront::bdd;

namespace {
using namespace webfront;
using namespace webfront::frontend::detail::webkitgtk;
}  // namespace

#if defined(__linux__)
const Scenario webKitGTKSnapPaths{"WebKitGTK detects inherited Snap toolkit paths", [] {
                                      then("recognizes path lists without mistaking unrelated paths for Snap", [] {
                                          check(containsSnapPath("/snap/core20/current/lib"));
                                          check(containsSnapPath("/usr/lib:/var/lib/snapd/snap/core/current/lib", ":;"));
                                          check(containsSnapPath("/usr/lib;/snap/core/current/lib", ":;"));
                                          check(containsSnapPath("/usr/lib/a.so /snap/core/current/lib/a.so", ": "));
                                          check(!containsSnapPath("/home/user/cache /snap/test/immodules.cache"));
                                          check(!containsSnapPath("/home/user/cache /snap/test/modules", ":"));
                                          check(!containsSnapPath("/usr/lib:/home/user/snap/app", ":"));
                                          check(!containsSnapPath("/snapshots/lib:/snap", ":"));
                                          check(!containsSnapPath(""));
                                      });
                                  }};

namespace {
// Runs `body` with every Snap-sensitive variable (and its saved original) removed, then restores the test process.
template<typename Body>
void withSnapEnvironment(Body&& body) {
    std::vector<std::pair<std::string, std::optional<std::string>>> saved;
    const auto remember = [&](const std::string& name) {
        const auto* value = std::getenv(name.c_str());
        saved.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
        unsetenv(name.c_str());
    };
    for (const auto& variable : snapSensitiveVariables) {
        remember(variable.name);
        remember(std::string(variable.name) + "_VSCODE_SNAP_ORIG");
    }
    body();
    for (const auto& [name, value] : saved) {
        if (value)
            setenv(name.c_str(), value->c_str(), 1);
        else
            unsetenv(name.c_str());
    }
}

std::string current(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "<unset>";
}
}  // namespace

const Scenario webKitGTKScrubsSnap{
    "WebKitGTK repairs an environment inherited from a Snap-hosted terminal", [] {
        then("unsets a single Snap path that breaks WebKit's GIO modules", [] {
            withSnapEnvironment([] {
                setenv("GIO_MODULE_DIR", "/home/user/snap/codium/common/.cache/gio-modules", 1);
                setenv("GTK_EXE_PREFIX", "/snap/codium/495/usr", 1);
                check(inheritedSnapToolkitPaths());
                const auto changed = scrubInheritedSnapEnvironment();
                check(changed == std::vector<std::string>{"GTK_EXE_PREFIX"});
                check(current("GIO_MODULE_DIR") == "/home/user/snap/codium/common/.cache/gio-modules");
                check(current("GTK_EXE_PREFIX") == "<unset>");
                check(!inheritedSnapToolkitPaths());
            });
        });
        then("restores the original value saved by the editor, empty meaning unset", [] {
            withSnapEnvironment([] {
                setenv("GIO_MODULE_DIR", "/snap/codium/495/usr/lib/gio", 1);
                setenv("GIO_MODULE_DIR_VSCODE_SNAP_ORIG", "", 1);
                setenv("XDG_DATA_DIRS", "/snap/codium/495/usr/share:/usr/share", 1);
                setenv("XDG_DATA_DIRS_VSCODE_SNAP_ORIG", "/usr/local/share:/usr/share", 1);
                scrubInheritedSnapEnvironment();
                check(current("GIO_MODULE_DIR") == "<unset>");
                check(current("XDG_DATA_DIRS") == "/usr/local/share:/usr/share");
            });
        });
        then("drops only the Snap entries of a list without a saved original", [] {
            withSnapEnvironment([] {
                setenv("LD_LIBRARY_PATH", "/opt/lib:/snap/core20/current/lib:/home/user/snap/app:/var/lib/snapd/snap/core/1/lib", 1);
                scrubInheritedSnapEnvironment();
                check(current("LD_LIBRARY_PATH") == "/opt/lib:/home/user/snap/app");
                setenv("XDG_CONFIG_DIRS", "/snap/codium/495/etc", 1);
                scrubInheritedSnapEnvironment();
                check(current("XDG_CONFIG_DIRS") == "<unset>");
            });
        });
        then("repairs a per-user directory the editor overrode, found through its saved original", [] {
            withSnapEnvironment([] {
                setenv("GIO_MODULE_DIR", "/home/user/snap/codium/common/.cache/gio-modules", 1);
                setenv("GIO_MODULE_DIR_VSCODE_SNAP_ORIG", "", 1);
                check(inheritedSnapToolkitPaths());
                check(scrubInheritedSnapEnvironment() == std::vector<std::string>{"GIO_MODULE_DIR"});
                check(current("GIO_MODULE_DIR") == "<unset>");
            });
        });
        then("leaves a variable the editor saved unchanged", [] {
            withSnapEnvironment([] {
                setenv("GTK_PATH", "/usr/lib/gtk-3.0", 1);
                setenv("GTK_PATH_VSCODE_SNAP_ORIG", "/usr/lib/gtk-3.0", 1);
                check(!inheritedSnapToolkitPaths());
                check(scrubInheritedSnapEnvironment().empty());
            });
        });
        then("leaves a clean environment untouched", [] {
            withSnapEnvironment([] {
                setenv("GTK_PATH", "/usr/lib/x86_64-linux-gnu/gtk-3.0", 1);
                check(scrubInheritedSnapEnvironment().empty());
                check(current("GTK_PATH") == "/usr/lib/x86_64-linux-gnu/gtk-3.0");
            });
        });
    }};
#endif

const Scenario webKitGTKApplies{"WebKitGTK applies navigation policy without toolkit objects", [] {
                                    const detail::BoundContentPolicy policy;
                                    struct Case {
                                        PolicyEvent    event;
                                        const char*    uri;
                                        bool           mainResource;
                                        unsigned       status;
                                        bool           initial;
                                        PolicyDecision expected;
                                    };
                                    const auto cases = std::to_array<Case>({
                                        {     PolicyEvent::navigation,    "http://127.0.0.1:9002/page", false,   0, false,                 PolicyDecision::allow},
                                        {     PolicyEvent::navigation,    "https://other.example/page", false,   0, false,                 PolicyDecision::allow},
                                        {     PolicyEvent::navigation,                   "about:blank", false,   0, false,                 PolicyDecision::allow},
                                        {     PolicyEvent::navigation,                  "about:srcdoc", false,   0, false,                 PolicyDecision::allow},
                                        {     PolicyEvent::navigation,          "data:text/html,hello", false,   0, false,                PolicyDecision::cancel},
                                        {     PolicyEvent::navigation, "blob:http://127.0.0.1:9002/id", false,   0, false,                 PolicyDecision::allow},
                                        {     PolicyEvent::navigation,                  "blob:null/id", false,   0, false,                PolicyDecision::cancel},
                                        {     PolicyEvent::navigation,            "file:///etc/passwd", false,   0, false,                PolicyDecision::cancel},
                                        {     PolicyEvent::navigation,                         nullptr, false,   0, false,                PolicyDecision::cancel},
                                        {       PolicyEvent::response,    "https://other.example/page", false, 403,  true,                 PolicyDecision::allow},
                                        {       PolicyEvent::response,                         nullptr, false,   0,  true,                 PolicyDecision::allow},
                                        {       PolicyEvent::response,    "https://other.example/page",  true, 200, false,                PolicyDecision::cancel},
                                        {       PolicyEvent::response,                         nullptr,  true, 200, false,                PolicyDecision::cancel},
                                        {       PolicyEvent::response,    "http://127.0.0.1:9002/page",  true, 200,  true,                 PolicyDecision::allow},
                                        {       PolicyEvent::response,    "http://127.0.0.1:9002/page",  true, 399,  true,                 PolicyDecision::allow},
                                        {       PolicyEvent::response,    "http://127.0.0.1:9002/page",  true, 400,  true, PolicyDecision::failInitialNavigation},
                                        {       PolicyEvent::response,    "http://127.0.0.1:9002/page",  true, 500, false,                 PolicyDecision::allow},
                                        {      PolicyEvent::newWindow,    "https://other.example/page", false,   0, false,                 PolicyDecision::popup},
                                        {      PolicyEvent::newWindow,                         nullptr, false,   0, false,                 PolicyDecision::popup},
                                        {static_cast<PolicyEvent>(99),                         nullptr, false,   0, false,                PolicyDecision::cancel},
                                    });
                                    then("responses enforce main-document origins and initial HTTP failures while actions reject opaque schemes", [&] {
                                        for (const auto& entry : cases) {
                                            auto scope = info(entry.uri ? entry.uri : "null URI");
                                            check(decidePolicy(policy, {entry.event, entry.uri, entry.mainResource, entry.status, entry.initial})
                                                  == entry.expected);
                                        }
                                    });
                                    given("an explicitly allowed destination", [&] {
                                        ContentPolicy options;
                                        options.navigationAllowedOrigins = {"https://other.example"};
                                        const detail::BoundContentPolicy allowed("http://127.0.0.1:9002", options);
                                        then("its redirected response may commit", [&] {
                                            check(decidePolicy(allowed, {PolicyEvent::response, "https://other.example/redirect", true, 200, true})
                                                  == PolicyDecision::allow);
                                        });
                                    });
                                }};

const Scenario webKitGTKRetains{"WebKitGTK retains the first session failure", [] {
                                    FailureState state;
                                    checkNothrow([&] {
                                        state.rethrowIfFailed();
                                    });
                                    when("a navigation error is followed by a queued timeout and callback exception", [&] {
                                        state.fail("original navigation error");
                                        const auto original = state.failure;
                                        state.fail("initial navigation timed out");
                                        try {
                                            throw std::logic_error("late callback failure");
                                        } catch (...) {
                                            state.rememberFailure();
                                        }
                                        then("the original exception remains observable", [&] {
                                            require(state.failure == original);
                                            requireThrows(
                                                [&] {
                                                    state.rethrowIfFailed();
                                                },
                                                equals("WebKitGTK: original navigation error"));
                                        });
                                    });
                                    when("the first failure came from a callback", [&] {
                                        try {
                                            throw std::logic_error("original callback failure");
                                        } catch (...) {
                                            state.rememberFailure();
                                        }
                                        state.fail("late timeout");
                                        then("a later native failure preserves the exception type and message", [&] {
                                            requireThrows<std::logic_error>([&] {
                                                state.rethrowIfFailed();
                                            });
                                            requireThrows(
                                                [&] {
                                                    state.rethrowIfFailed();
                                                },
                                                equals("original callback failure"));
                                        });
                                    });
                                }};

const Scenario webKitGTKHandles{"WebKitGTK handles missing native navigation diagnostics", [] {
                                    then("a null GError message has an actionable fallback", [&] {
                                        check(initialNavigationError(nullptr)
                                              == "initial navigation failed: unknown error; check the local server and page path");
                                        check(initialNavigationError("connection refused")
                                              == "initial navigation failed: connection refused; check the local server and page path");
                                    });
                                }};

#if defined(__linux__)
namespace {
class SandboxEnvironment {
    std::optional<std::string>        optOut   = read("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
    std::optional<std::string>        required = read("WEBFRONT_REQUIRE_WEBKIT_SANDBOX");
    static std::optional<std::string> read(const char* name) {
        if (const auto* value = std::getenv(name))
            return std::string(value);
        return std::nullopt;
    }
    static void restore(const char* name, const std::optional<std::string>& value) {
        if (value)
            setenv(name, value->c_str(), 1);
        else
            unsetenv(name);
    }

public:
    SandboxEnvironment() {
        unsetenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
        unsetenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX");
    }
    ~SandboxEnvironment() {
        restore("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", optOut);
        restore("WEBFRONT_REQUIRE_WEBKIT_SANDBOX", required);
    }
};

std::filesystem::path fakeLauncher(const std::filesystem::path& folder, std::string_view name, std::string_view message, int status) {
    const auto    path = folder / name;
    std::ofstream script(path);
    script << "#!/bin/sh\n";
    if (!message.empty())
        script << "echo '" << message << "' >&2\n";
    script << "exit " << status << "\n";
    script.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}
constexpr const char* sandboxOptOut = "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS";
}  // namespace

const Scenario webKitGTKDevelopment{"WebKitGTK development opens without a sandbox or a launcher", [] {
                                        const SandboxEnvironment                 environment;
                                        const webfront::test::TemporaryDirectory folder;
                                        then("the development policy works even without bubblewrap and overrides the upstream value 0", [&] {
                                            setenv(sandboxOptOut, "0", 1);
                                            require(prepareSandboxLaunch(folder.path() / "missing", SandboxMode::development).has_value());
                                            check(std::string(std::getenv(sandboxOptOut)) == "0");
                                            {
                                                DevelopmentSandboxEnvironment scope(SandboxMode::development);
                                                check(std::string(std::getenv(sandboxOptOut)) == "1");
                                                check(!sandboxLauncherApplies());
                                            }
                                            check(std::string(std::getenv(sandboxOptOut)) == "0");
                                            unsetenv(sandboxOptOut);
                                            try {
                                                DevelopmentSandboxEnvironment scope(SandboxMode::development);
                                                throw std::runtime_error("display failure");
                                            } catch (const std::runtime_error&) { }
                                            check(std::getenv(sandboxOptOut) == nullptr);
                                        });
                                    }};

const Scenario webKitGTKDistributionConflict{"WebKitGTK distribution refuses an inherited sandbox opt-out", [] {
                                                 const SandboxEnvironment                 environment;
                                                 const webfront::test::TemporaryDirectory folder;
                                                 then("the distribution build cannot be weakened by its environment", [&] {
                                                     setenv(sandboxOptOut, "1", 1);
                                                     setenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX", "0", 1);
                                                     requireThrows([&] {
                                                         prepareSandboxLaunch(folder.path() / "missing", SandboxMode::distribution);
                                                     });
                                                     check(std::string(std::getenv(sandboxOptOut)) == "1");
                                                 });
                                                 then("the legacy requirement also rejects the contradictory environment in development", [&] {
                                                     setenv(sandboxOptOut, "1", 1);
                                                     setenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX", "1", 1);
                                                     requireThrows([&] {
                                                         prepareSandboxLaunch(folder.path() / "missing", SandboxMode::development);
                                                     });
                                                 });
                                             }};

const Scenario webKitGTKLauncher{"WebKitGTK distribution diagnoses a refused namespace without disabling the sandbox", [] {
                                     const SandboxEnvironment                 environment;
                                     const webfront::test::TemporaryDirectory folder;
                                     const auto refused = fakeLauncher(folder.path(), "refused", "bwrap: setting up uid map: Permission denied", 1);
                                     then("the real subprocess diagnostic is returned", [&] {
                                         const auto refusal = sandboxLauncherFailure(refused);
                                         require(refusal.has_value());
                                         check(*refusal == "bwrap: setting up uid map: Permission denied");
                                     });
                                     then("the upstream value 0 keeps the launcher check active", [&] {
                                         setenv(sandboxOptOut, "0", 1);
                                         check(!webKitSandboxDisabled());
                                         // Flatpak and Snap delegate isolation to their package sandbox.
                                         if (!sandboxLauncherApplies())
                                             return;
                                         requireThrows([&] {
                                             prepareSandboxLaunch(refused, SandboxMode::distribution);
                                         });
                                         check(std::string(std::getenv(sandboxOptOut)) == "0");
                                         setenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX", "1", 1);
                                         requireThrows([&] {
                                             prepareSandboxLaunch(refused, SandboxMode::development);
                                         });
                                         check(std::string(std::getenv(sandboxOptOut)) == "0");
                                     });
                                 }};

const Scenario webKitGTKWorkingLauncher{
    "WebKitGTK distribution preserves a working sandbox",
    [] {
        const SandboxEnvironment                 environment;
        const webfront::test::TemporaryDirectory folder;
        then("a successful probe leaves the upstream environment unchanged", [&] {
            const auto working = fakeLauncher(folder.path(), "working", "", 0);
            check(!prepareSandboxLaunch(working, SandboxMode::distribution));
            check(std::getenv(sandboxOptOut) == nullptr);
            check(!sandboxLauncherFailure(working));
            setenv(sandboxOptOut, "1", 1);
            requireThrows([] {
                requireSandboxEnvironment(SandboxMode::distribution);
            });
            unsetenv(sandboxOptOut);
            check(!sandboxLauncherFailure(fakeLauncher(folder.path(), "noisy", "warning: namespace already shared", 0)));
        });
        then("missing launchers and every failed probe reject distribution", [&] {
            check(sandboxLauncherFailure(folder.path() / "missing").has_value());
            check(sandboxLauncherFailure(fakeLauncher(folder.path(), "other", "bwrap: Unknown option --ro-bind", 1)).has_value());
            check(sandboxLauncherFailure(fakeLauncher(folder.path(), "silent", "", 1)).has_value());
            check(std::getenv(sandboxOptOut) == nullptr);
        });
    }};
const Scenario webKitGTKProbeLifecycle{"WebKitGTK probes tolerate host child management and enforce deadlines", [] {
                                           const webfront::test::TemporaryDirectory folder;
                                           const auto                               working = fakeLauncher(folder.path(), "success", "", 0);
                                           then("SIGCHLD ignored by the host does not turn a healthy probe into a refusal", [&] {
                                               const auto previous = signal(SIGCHLD, SIG_IGN);
                                               const auto failure  = sandboxLauncherFailure(working);
                                               signal(SIGCHLD, previous);
                                               check(!failure);
                                           });
                                           then("a host reaper cannot steal the launcher status", [&] {
                                               const auto previous = signal(
                                                   SIGCHLD,
                                                   +[](int) {
                                                       const int savedError = errno;
                                                       int       status;
                                                       while (waitpid(-1, &status, WNOHANG) > 0) { }
                                                       errno = savedError;
                                                   });
                                               const auto failure = sandboxLauncherFailure(working);
                                               signal(SIGCHLD, previous);
                                               check(!failure);
                                           });
                                           then("a stalled launcher is killed at its deadline", [&] {
                                               const auto path = folder.path() / "stalled";
                                               std::ofstream(path) << "#!/bin/sh\nsleep 30\n";
                                               std::filesystem::permissions(path, std::filesystem::perms::owner_all);
                                               const auto start   = std::chrono::steady_clock::now();
                                               const auto failure = sandboxLauncherFailure(path, std::chrono::milliseconds(50));
                                               require(failure.has_value());
                                               check(failure->find("timed out") != std::string::npos);
                                               check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
                                           });
                                       }};

const Scenario webKitGTKContainerFallback{"WebKitGTK distribution rejects WebKit container fallback", [] {
                                              const SandboxEnvironment environment;
                                              then("a successful preflight cannot authorize a later unsandboxed fallback", [] {
                                                  requireThrows([] {
                                                      requireSandboxEnvironment(SandboxMode::distribution, true);
                                                  });
                                                  requireSandboxEnvironment(SandboxMode::development, true);
                                                  setenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX", "1", 1);
                                                  requireThrows([] {
                                                      requireSandboxEnvironment(SandboxMode::development, true);
                                                  });
                                              });
                                          }};

const Scenario webKitGTKUnverifiedSnap{"WebKitGTK distribution rejects inherited Snap claims", [] {
                                           const SandboxEnvironment   environment;
                                           std::optional<std::string> previous[3];
                                           const char*                names[] = {"SNAP", "SNAP_NAME", "SNAP_REVISION"};
                                           for (int i = 0; i < 3; ++i) {
                                               if (const auto* value = std::getenv(names[i]))
                                                   previous[i] = value;
                                               setenv(names[i], "webfront-test-not-a-real-snap", 1);
                                           }
                                           then("environment alone cannot delegate the sandbox", [] {
                                               check(!snapConfinementVerified());
                                               requireThrows([] {
                                                   requireSandboxEnvironment(SandboxMode::distribution);
                                               });
                                           });
                                           for (int i = 0; i < 3; ++i) {
                                               if (previous[i])
                                                   setenv(names[i], previous[i]->c_str(), 1);
                                               else
                                                   unsetenv(names[i]);
                                           }
                                       }};

#endif
