#include "Bdd.hpp"
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
// A stand-in for bubblewrap: a script with the given standard error and exit status.
std::filesystem::path fakeLauncher(std::string_view name, std::string_view message, int status) {
    const auto folder = std::filesystem::temp_directory_path() / ("webfront-launcher-" + std::to_string(getpid()));
    std::filesystem::create_directories(folder);
    const auto path = folder / name;
    {
        std::ofstream script(path);
        script << "#!/bin/sh\n";
        if (!message.empty())
            script << "echo '" << message << "' >&2\n";
        script << "exit " << status << "\n";
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return path;
}
}  // namespace

const Scenario webKitGTKSandboxLauncher{"WebKitGTK opens its window where the system refuses the sandbox launcher", [] {
    const auto* optOut = "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS";
    unsetenv(optOut);
    unsetenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX");
    const auto refused = fakeLauncher("refused", "bwrap: setting up uid map: Permission denied", 1);
    when("the launcher cannot set up its user namespace", [&] {
        then("its message is returned", [&] {
            const auto refusal = sandboxLauncherRefusal(refused);
            require(refusal.has_value());
            check(*refusal == "bwrap: setting up uid map: Permission denied");
        });
        if (!sandboxLauncherApplies())
            return;  // inside a container WebKit does not use the launcher
        then("the web process is launched without it, instead of letting WebKit abort the process", [&] {
            const auto refusal = prepareSandboxLaunch(refused);
            require(refusal.has_value());
            check(*refusal == "bwrap: setting up uid map: Permission denied");
            require(std::getenv(optOut) != nullptr);
            check(std::string(std::getenv(optOut)) == "1");
            check(!sandboxLauncherApplies());
            unsetenv(optOut);
        });
        then("a deployment that requires the sandbox gets an initialization error", [&] {
            setenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX", "1", 1);
            requireThrows([&] { prepareSandboxLaunch(refused); },
                          equals("WebKitGTK: the web process sandbox cannot start (bwrap: setting up uid map: Permission denied) and "
                                 "WEBFRONT_REQUIRE_WEBKIT_SANDBOX is set; see docs/WEBKITGTK.md, \"Sandbox and user namespaces\""));
            check(std::getenv(optOut) == nullptr);
            unsetenv("WEBFRONT_REQUIRE_WEBKIT_SANDBOX");
        });
    });
    when("the launcher works, is absent or fails for another reason", [&] {
        then("nothing is concluded and WebKit keeps its sandbox", [&] {
            check(!prepareSandboxLaunch(fakeLauncher("working", "", 0)));
            check(std::getenv(optOut) == nullptr);
            check(!sandboxLauncherRefusal(fakeLauncher("working", "", 0)));
            check(!sandboxLauncherRefusal(fakeLauncher("noisy", "warning: namespace already shared", 0)));
            check(!sandboxLauncherRefusal(fakeLauncher("other", "bwrap: Unknown option --ro-bind", 1)));
            check(!sandboxLauncherRefusal(refused.parent_path() / "missing"));
        });
    });
    std::filesystem::remove_all(refused.parent_path());
}};
#endif
