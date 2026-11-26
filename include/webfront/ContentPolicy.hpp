// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-18
//
// WebFront — core
// Role: Shared per-application content rules (origins, popups, navigation, deployment headers) applied by every frontend and the HTTP server.
// Thread: worker-safe | Alloc: heap-init
// LLM: public API; see docs/CONTENT_POLICY.md before changing defaults

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront {

enum class PopupPolicy { block, systemBrowser };

/** @brief Shared per-application content rules. See docs/CONTENT_POLICY.md for scope and defaults. */
struct ContentPolicy {
    std::vector<std::string> bridgeAllowedOrigins{};
    std::vector<std::string> navigationAllowedOrigins{};
    bool                     allowAnyBridgeOrigin{false};
    bool                     allowAnyNavigation{false};
    bool                     allowMissingOrigin{true};
    PopupPolicy              popups{PopupPolicy::block};
    bool                     devToolsEnabled{true};
    bool                     contextMenusEnabled{true};
    /// `Referrer-Policy` of every reply the HTTP server writes, unless the reply sets its own; empty: no header.
    std::string referrerPolicy{};
    /// `X-Content-Type-Options: nosniff` on every reply the HTTP server writes, unless the reply sets its own.
    bool noSniff{false};
    /// `Cache-Control` of the HTML files the HTTP server serves (not the routes, which set their own); empty: no header.
    std::string htmlCacheControl{};
};

namespace detail {

inline std::string lowerOriginText(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char c) -> char {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    });
    return result;
}

struct OriginAuthority {
    std::string_view host;
    std::string_view port;
};

inline std::optional<OriginAuthority> ipv6OriginAuthority(std::string_view authority) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1)
        return {};
    const auto host = authority.substr(0, close + 1);
    if (host.substr(1, close - 1).find_first_not_of("0123456789abcdefABCDEF:.") != std::string_view::npos)
        return {};
    if (host.find(':') == std::string_view::npos)
        return {};
    const auto suffix = authority.substr(close + 1);
    if (suffix.empty())
        return OriginAuthority{host, {}};
    if (!suffix.starts_with(':') || suffix.size() == 1)
        return {};
    return OriginAuthority{host, suffix.substr(1)};
}

inline std::optional<OriginAuthority> originAuthority(std::string_view authority) {
    if (authority.empty())
        return {};
    if (authority.front() == '[')
        return ipv6OriginAuthority(authority);
    const auto colon = authority.find(':');
    const auto host  = authority.substr(0, colon);
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.") != std::string_view::npos)
        return {};
    if (colon == std::string_view::npos)
        return OriginAuthority{host, {}};
    const auto port = authority.substr(colon + 1);
    if (port.empty())
        return {};
    return OriginAuthority{host, port};
}

inline std::optional<unsigned> originPort(std::string_view port, unsigned defaultPort) {
    if (port.empty())
        return defaultPort;
    if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string_view::npos)
        return {};
    unsigned number = 0;
    for (char digit : port)
        number = number * 10 + static_cast<unsigned>(digit - '0');
    if (number > 65535)
        return {};
    return number;
}

struct OriginUrlParts {
    std::string      scheme;
    std::string_view authority;
};

inline std::optional<OriginUrlParts> originUrlParts(std::string_view url, bool originOnly) {
    const auto separator = url.find("://");
    if (separator == std::string_view::npos)
        return {};
    auto scheme = lowerOriginText(url.substr(0, separator));
    if (scheme != "http" && scheme != "https")
        return {};
    if (url.find_first_of("\\\r\n\t ") != std::string_view::npos)
        return {};
    const auto authorityText = url.substr(separator + 3);
    const auto end           = authorityText.find_first_of("/?#");
    if (originOnly && end != std::string_view::npos)
        return {};
    return OriginUrlParts{std::move(scheme), authorityText.substr(0, end)};
}

inline std::string serializedContentOrigin(std::string_view scheme, std::string_view host, unsigned port, unsigned defaultPort) {
    auto result = std::string(scheme) + "://" + lowerOriginText(host);
    if (port != defaultPort)
        result += ":" + std::to_string(port);
    return result;
}

// HTTP(S) origins only. Do not compare URL prefixes or trust the request's Host header.
// Fail closed on opaque origins, credentials, escapes and ambiguous URL syntax.
inline std::optional<std::string> contentOrigin(std::string_view url, bool originOnly = false) {
    const auto parts = originUrlParts(url, originOnly);
    if (!parts)
        return {};
    const auto authority = originAuthority(parts->authority);
    if (!authority)
        return {};
    const unsigned defaultPort = parts->scheme == "https" ? 443 : 80;
    const auto     port        = originPort(authority->port, defaultPort);
    if (!port)
        return {};
    return serializedContentOrigin(parts->scheme, authority->host, *port, defaultPort);
}

/**
 * @brief The URL a `blob:` URL was minted for (`blob:http://host:port/id` gives `http://host:port/id`), or empty.
 *
 * A page downloads a generated `Blob` by navigating to such a URL; its content always belongs to the page's own origin,
 * so the origin it names is the one the navigation policy must judge.
 */
inline std::string_view blobOriginUrl(std::string_view url) {
    constexpr std::string_view scheme = "blob:";
    if (url.size() <= scheme.size() || !std::ranges::equal(url.substr(0, scheme.size()), scheme, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        }))
        return {};
    return url.substr(scheme.size());
}

/** @brief Validated snapshot shared by the HTTP and embedded-frontend policy hooks. */
class BoundContentPolicy {
public:
    explicit BoundContentPolicy(std::string_view applicationUrl = "http://127.0.0.1:9002/", ContentPolicy options = {})
        : policy(std::move(options)), applicationOrigin(requireOrigin(applicationUrl, false)) {
        normalize(policy.bridgeAllowedOrigins);
        normalize(policy.navigationAllowedOrigins);
        requireReferrerPolicy(policy.referrerPolicy);
        requireFieldValue("htmlCacheControl", policy.htmlCacheControl);
    }

    /// Throws std::invalid_argument for what the constructor would refuse in `options`, without an application URL: the server
    /// calls it before it binds its port, so a bad configuration is never reported as a port in use.
    static void validate(const ContentPolicy& options) {
        for (const auto* origins : {&options.bridgeAllowedOrigins, &options.navigationAllowedOrigins})
            for (const auto& origin : *origins)
                (void)requireOrigin(origin);
        requireReferrerPolicy(options.referrerPolicy);
        requireFieldValue("htmlCacheControl", options.htmlCacheControl);
    }

    bool allowsBridge(std::optional<std::string_view> origin) const {
        if (!origin)
            return policy.allowMissingOrigin;
        const auto parsed = contentOrigin(*origin, true);
        return parsed && (policy.allowAnyBridgeOrigin || *parsed == applicationOrigin || contains(policy.bridgeAllowedOrigins, *parsed));
    }

    bool allowsNavigation(std::string_view url) const {
        if (const auto inner = blobOriginUrl(url); !inner.empty())
            url = inner;  // A Blob of an admitted origin (a generated download) is that origin's own content.
        const auto origin = contentOrigin(url);
        return origin && (policy.allowAnyNavigation || *origin == applicationOrigin || contains(policy.navigationAllowedOrigins, *origin));
    }

    /// Whether `url` belongs to the application's own origin, whatever the navigation allow-list admits.
    bool isApplicationOrigin(std::string_view url) const {
        const auto origin = contentOrigin(url);
        return origin && *origin == applicationOrigin;
    }

    // Popups always leave the embedded session. Only HTTP(S) is forwarded, never shell/file schemes.
    template <typename Launcher>
    void handlePopup(const std::string& url, bool userInitiated, Launcher&& launch) const {
        if (!userInitiated)
            return;
        if (policy.popups == PopupPolicy::systemBrowser && contentOrigin(url))
            std::forward<Launcher>(launch)(url);
    }

    const ContentPolicy& options() const noexcept {
        return policy;
    }

    /// Calls `visit(name, value)` for each header every HTTP reply carries unless it sets that header itself.
    template <typename Visitor>
    void forEachResponseHeader(Visitor&& visit) const {
        if (!policy.referrerPolicy.empty())
            visit(std::string_view{"Referrer-Policy"}, std::string_view{policy.referrerPolicy});
        if (policy.noSniff)
            visit(std::string_view{"X-Content-Type-Options"}, std::string_view{"nosniff"});
    }

private:
    ContentPolicy policy;
    std::string   applicationOrigin;

    static std::string requireOrigin(std::string_view value, bool originOnly = true) {
        if (auto origin = contentOrigin(value, originOnly))
            return *origin;
        throw std::invalid_argument("WebFront: content policy requires an HTTP(S) origin: " + std::string(value));
    }
    // The tokens of the Referrer Policy specification; a comma-separated list is a fallback chain for older browsers.
    static void requireReferrerPolicy(std::string_view value) {
        static constexpr std::array<std::string_view, 8> tokens{"no-referrer",
                                                                "no-referrer-when-downgrade",
                                                                "origin",
                                                                "origin-when-cross-origin",
                                                                "same-origin",
                                                                "strict-origin",
                                                                "strict-origin-when-cross-origin",
                                                                "unsafe-url"};
        if (value.empty())
            return;
        for (std::size_t start = 0;;) {
            const auto comma = value.find(',', start);
            auto       token = value.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                token.remove_suffix(1);
            if (std::ranges::find(tokens, token) == tokens.end())
                throw std::invalid_argument("WebFront: contentPolicy.referrerPolicy is not a Referrer-Policy value: " + std::string(value));
            if (comma == std::string_view::npos)
                return;
            start = comma + 1;
        }
    }
    // A header value that cannot split a response: no control character but the tab (RFC 9110 §5.5).
    static void requireFieldValue(std::string_view member, std::string_view value) {
        if (std::ranges::any_of(value, [](char c) {
                const auto byte = static_cast<unsigned char>(c);
                return (byte < 0x20 && c != '\t') || byte == 0x7f;
            }))
            throw std::invalid_argument("WebFront: contentPolicy." + std::string(member) + " holds a control character");
    }
    static void normalize(std::vector<std::string>& origins) {
        for (auto& origin : origins)
            origin = requireOrigin(origin);
    }
    static bool contains(const std::vector<std::string>& origins, const std::string& value) {
        return std::ranges::find(origins, value) != origins.end();
    }
};

}  // namespace detail
}  // namespace webfront
