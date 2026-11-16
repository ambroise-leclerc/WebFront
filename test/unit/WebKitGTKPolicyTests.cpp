#include "Bdd.hpp"
#include "TemporaryDirectory.hpp"
#include <webfront/frontend/WebKitGTKPolicy.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__linux__)
    #include <unistd.h>
#endif

using namespace webfront::bdd;

namespace {
using namespace webfront;
using namespace webfront::frontend::detail::webkitgtk;
}  // namespace

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
        {     PolicyEvent::navigation,                   "blob:null/id", false,   0, false,                PolicyDecision::cancel},
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
            check(decidePolicy(policy, {entry.event, entry.uri, entry.mainResource, entry.status, entry.initial}) == entry.expected);
        }
    });
    given("an explicitly allowed destination", [&] {
        ContentPolicy options;
        options.navigationAllowedOrigins = {"https://other.example"};
        const detail::BoundContentPolicy allowed("http://127.0.0.1:9002", options);
        then("its redirected response may commit", [&] {
            check(decidePolicy(allowed, {PolicyEvent::response, "https://other.example/redirect", true, 200, true}) == PolicyDecision::allow);
        });
    });
}};

const Scenario webKitGTKRetains{"WebKitGTK retains the first session failure", [] {
    FailureState state;
    checkNothrow([&] { state.rethrowIfFailed(); });
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
            requireThrows([&] { state.rethrowIfFailed(); }, equals("WebKitGTK: original navigation error"));
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
            requireThrows<std::logic_error>([&] { state.rethrowIfFailed(); });
            requireThrows([&] { state.rethrowIfFailed(); }, equals("original callback failure"));
        });
    });
}};

const Scenario webKitGTKHandles{"WebKitGTK handles missing native navigation diagnostics", [] {
    then("a null GError message has an actionable fallback", [&] {
        check(initialNavigationError(nullptr) == "initial navigation failed: unknown error; check the local server and page path");
        check(initialNavigationError("connection refused") == "initial navigation failed: connection refused; check the local server and page path");
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
                                            check(std::string(std::getenv(sandboxOptOut)) == "1");
                                            check(!sandboxLauncherApplies());
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
                                         const auto refusal = sandboxLauncherRefusal(refused);
                                         require(refusal.has_value());
                                         check(*refusal == "bwrap: setting up uid map: Permission denied");
                                     });
                                     then("the upstream value 0 keeps the launcher check active", [&] {
                                         setenv(sandboxOptOut, "0", 1);
                                         check(!webKitSandboxDisabled());
                                         // Containers use the engine's outer-sandbox integration instead of bubblewrap.
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

const Scenario webKitGTKWorkingLauncher{"WebKitGTK distribution preserves a working sandbox", [] {
                                            const SandboxEnvironment                 environment;
                                            const webfront::test::TemporaryDirectory folder;
                                            then("a successful probe leaves the upstream environment unchanged", [&] {
                                                const auto working = fakeLauncher(folder.path(), "working", "", 0);
                                                check(!prepareSandboxLaunch(working, SandboxMode::distribution));
                                                check(std::getenv(sandboxOptOut) == nullptr);
                                                check(!sandboxLauncherRefusal(working));
                                                check(!sandboxLauncherRefusal(fakeLauncher(folder.path(), "noisy", "warning: namespace already shared", 0)));
                                            });
                                            then("missing launchers and unrelated failures are left for WebKit to diagnose", [&] {
                                                check(!sandboxLauncherRefusal(folder.path() / "missing"));
                                                check(!sandboxLauncherRefusal(fakeLauncher(folder.path(), "other", "bwrap: Unknown option --ro-bind", 1)));
                                                check(std::getenv(sandboxOptOut) == nullptr);
                                            });
                                        }};
#endif
