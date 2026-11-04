/// @brief Proof that a connection comes from the view of an embedded frontend of this application (ADR-020, rule 3).
///
/// An address proves nothing: a remote browser behind a local proxy or an SSH tunnel, or another user of the machine,
/// also arrives from the loopback. The adapter of an embedded frontend therefore hands a per-launch secret to its own
/// view, in the process, and WebFront.js presents it in the WebSocket handshake. The secret is random, compared in
/// constant time, never served over HTTP and never given to the system browser (a command line or a history could
/// disclose it, and a remote browser would present it first). It does not protect against a script admitted into the
/// view (ADR-008): such a script shares the authority of the bridge and could read the secret before WebFront.js.
#pragma once

#include "ApplicationOrigin.hpp"
#include "automation/Credentials.hpp"

#include <algorithm>
#include <cctype>
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

/// True when `presented` is the secret of this launch. Every byte of the launch secret is compared whatever the presented
/// bytes are, and a difference of length is folded into the result, so the time depends only on the (public) secret length.
[[nodiscard]] inline bool viewSecretMatches(std::string_view launchSecret, std::string_view presented) noexcept {
    if (launchSecret.empty())
        return false;
    std::size_t mismatch = launchSecret.size() ^ presented.size();
    for (std::size_t index = 0; index < launchSecret.size(); ++index) {
        const unsigned char given = index < presented.size() ? static_cast<unsigned char>(presented[index]) : 0;
        mismatch |= static_cast<std::size_t>(static_cast<unsigned char>(launchSecret[index]) ^ given);
    }
    return mismatch == 0;
}

/// The origin as the browser writes `location.origin`: lowercase, without the default port 80. The comparison of the script
/// below is a string comparison, so an origin written otherwise would never match and the view would stay unassociated.
[[nodiscard]] inline std::string browserOrigin(std::string_view address, std::string_view port) {
    auto origin = applicationOrigin(address, port);
    std::ranges::transform(origin, origin.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (origin.ends_with(":80"))
        origin.resize(origin.size() - 3);
    return origin;
}

/// The JavaScript that gives `secret` to the document when, and only when, it is a top-level document of origin `origin`.
/// Returns an empty script (no association) unless `secret` is lowercase hexadecimal and `origin` is made of the characters
/// of a plain http origin: nothing else can be written in a string literal without escaping, and nothing is escaped here.
[[nodiscard]] inline std::string viewSecretScript(std::string_view secret, std::string_view origin) {
    const auto isHex    = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    const auto isOrigin = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || std::string_view{":/.[]-"}.contains(c); };
    if (secret.empty() || origin.empty() || !std::ranges::all_of(secret, isHex) || !std::ranges::all_of(origin, isOrigin))
        return {};
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
