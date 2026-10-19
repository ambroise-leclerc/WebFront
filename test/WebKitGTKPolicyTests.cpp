#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <frontend/WebKitGTKPolicy.hpp>

#include <array>

namespace {
using namespace webfront;
using namespace webfront::frontend::detail::webkitgtk;
}  // namespace

SCENARIO("WebKitGTK applies navigation policy without toolkit objects") {
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
        {     PolicyEvent::navigation, "blob:http://127.0.0.1:9002/id", false,   0, false,                PolicyDecision::cancel},
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
    THEN("responses enforce main-document origins and initial HTTP failures while actions reject opaque schemes") {
        for (const auto& entry : cases) {
            INFO((entry.uri ? entry.uri : "null URI"));
            CHECK(decidePolicy(policy, {entry.event, entry.uri, entry.mainResource, entry.status, entry.initial}) == entry.expected);
        }
    }
    GIVEN("an explicitly allowed destination") {
        ContentPolicy options;
        options.navigationAllowedOrigins = {"https://other.example"};
        const detail::BoundContentPolicy allowed("http://127.0.0.1:9002", options);
        THEN("its redirected response may commit") {
            CHECK(decidePolicy(allowed, {PolicyEvent::response, "https://other.example/redirect", true, 200, true}) == PolicyDecision::allow);
        }
    }
}

SCENARIO("WebKitGTK retains the first session failure") {
    FailureState state;
    CHECK_NOTHROW(state.rethrowIfFailed());
    WHEN("a navigation error is followed by a queued timeout and callback exception") {
        state.fail("original navigation error");
        const auto original = state.failure;
        state.fail("initial navigation timed out");
        try {
            throw std::logic_error("late callback failure");
        } catch (...) {
            state.rememberFailure();
        }
        THEN("the original exception remains observable") {
            REQUIRE(state.failure == original);
            REQUIRE_THROWS_WITH(state.rethrowIfFailed(), "WebKitGTK: original navigation error");
        }
    }
    WHEN("the first failure came from a callback") {
        try {
            throw std::logic_error("original callback failure");
        } catch (...) {
            state.rememberFailure();
        }
        state.fail("late timeout");
        THEN("a later native failure preserves the exception type and message") {
            REQUIRE_THROWS_AS(state.rethrowIfFailed(), std::logic_error);
            REQUIRE_THROWS_WITH(state.rethrowIfFailed(), "original callback failure");
        }
    }
}

SCENARIO("WebKitGTK handles missing native navigation diagnostics") {
    THEN("a null GError message has an actionable fallback") {
        CHECK(initialNavigationError(nullptr) == "initial navigation failed: unknown error; check the local server and page path");
        CHECK(initialNavigationError("connection refused") == "initial navigation failed: connection refused; check the local server and page path");
    }
}
