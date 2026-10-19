#pragma once

#include "../ContentPolicy.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

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
    if (webfront::detail::contentOrigin(uri))
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

}  // namespace webfront::frontend::detail::webkitgtk
