/// @brief Proof that a connection comes from the view of an embedded frontend of this application (ADR-020, rule 3).
///
/// An address proves nothing: a remote browser behind a local proxy or an SSH tunnel, or another user of the machine,
/// also arrives from the loopback. The adapter of an embedded frontend therefore hands a per-launch secret to its own
/// view, in the process, and WebFront.js presents it in the WebSocket handshake. The secret is random, compared in
/// constant time, never served over HTTP and never given to the system browser (a command line or a history could
/// disclose it, and a remote browser would present it first). It does not protect against a script admitted into the
/// view (ADR-008): such a script shares the authority of the bridge and could read the secret before WebFront.js.
#pragma once

#include "automation/Credentials.hpp"

#include <string>
#include <string_view>

namespace webfront {

/// What an embedded frontend needs to prove its view: the secret to hand to the view, and the origin that may receive it.
struct ViewLaunch {
    /// Empty when the launch has no secret to give (a test double, an adapter that cannot inject one).
    std::string secret;
    /// The application's origin ("http://127.0.0.1:9002"): the adapter exposes the secret to documents of this origin only.
    std::string origin;
};

namespace detail {

/// The name of the property the adapter defines on the window of the view, and WebFront.js reads then removes.
inline constexpr std::string_view viewSecretProperty = "__webfrontView";

/// A fresh random secret: 64 hexadecimal characters (256 bits), valid for one application only.
inline std::string newViewSecret() {
    return automation::channel::nonce();
}

/// True when `presented` is the secret of this launch; the comparison takes the same time wherever they differ.
[[nodiscard]] inline bool viewSecretMatches(std::string_view launchSecret, std::string_view presented) noexcept {
    return !launchSecret.empty() && automation::channel::sameToken(launchSecret, presented);
}

/// The JavaScript that gives `secret` to the document when, and only when, it is a top-level document of origin `origin`. Both are
/// restricted to characters that need no escaping in a string literal.
[[nodiscard]] inline std::string viewSecretScript(std::string_view secret, std::string_view origin) {
    std::string script = "if (window === window.top && location.origin === '";
    script += origin;
    script += "') Object.defineProperty(window, '";
    script += viewSecretProperty;
    script += "', {value: '";
    script += secret;
    script += "', configurable: true});";
    return script;
}

}  // namespace detail
}  // namespace webfront
