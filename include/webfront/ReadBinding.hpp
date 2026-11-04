// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Binds the plain HTTP reads of a page (`<img>`, `<video>`, `fetch`, a download) to the browser client whose
/// connection received the read token (#469, ADR-020 rule 4).
///
/// A read URL carries a token, but an URL is a capability to whoever holds it, and `<img>` or `<video>` cannot add a header.
/// A bound read therefore also needs a cookie. After its handshake, a connection receives through the bridge a one-time
/// secret and exchanges it, by a same-origin `fetch`, for a `HttpOnly`, `SameSite=Strict` cookie named after the *client*
/// (the browser profile) it identifies. A cookie is shared by the tabs of a profile, so the binding designates a client, not
/// a connection; the connections of one profile that present the cookie belong to one client. Two concurrent exchanges of
/// a blank profile create two clients and two cookies of different names, which coexist.
///
/// The cookie proves possession, nothing more: another page of the same origin in the same profile reads the URL, as it
/// already shares storage, cookies and the authority of the bridge (ADR-008). It does not replace the origin check of the
/// bridge. The *transport* is declared, never deduced: only the view of an embedded frontend is local; any other
/// connection reads only if the application declared its deployment (RemoteFileTransport).
#pragma once

#include "ViewAssociation.hpp"
#include "automation/Credentials.hpp"
#include "http/HTTPServer.hpp"

#include <algorithm>
#include <any>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront {

/// How the application declares that a connection which is not the view of an embedded frontend (the system browser, a
/// remote browser) reaches it (ADR-020). WebFront decides on neither the address nor `Forwarded`/`X-Forwarded-*`.
enum class RemoteFileTransport {
    refused,                ///< Default: such a connection reads no file.
    loopbackAttested,       ///< The server listens on the loopback only (checked at configuration); the integrator attests no proxy or tunnel exposes it and the machine is not shared with untrusted users.
    encryptedByDeployment,  ///< The deployment encrypts (TLS reverse proxy, encrypted tunnel): unverified. The binding cookie is `Secure`.
    plaintextAccepted       ///< An isolated trusted network: unverified.
};

/// Refused grant or malformed read configuration.
struct ReadRefused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Options of one read URL.
struct ReadOptions {
    /// Lifetime of the token; zero means until the connection closes (or the grant is revoked). Mandatory, short and
    /// bounded (ReadBinding::maxTransferableLifetime) for a transferable grant.
    std::chrono::seconds lifetime{0};
    /// The token alone suffices, as a bearer capability: for handing the URL to an external application. Off by default.
    bool transferable = false;
    /// Handed back to the route handler with each request of this grant (a file handle, a path, a permission...).
    std::any context{};
};

/// What a read route handler learns about the grant a request presented.
struct ReadGrant {
    std::string_view rest;     ///< The percent-decoded path after the token, without a leading '/'.
    const std::any&  context;  ///< ReadOptions::context of the grant.
};

using ReadHandler = std::function<http::RouteReply(const http::RouteRequest&, const ReadGrant&)>;

namespace detail {

[[nodiscard]] inline std::string lowerAscii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] inline std::string_view trimBlanks(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

/// The `name=value` pairs of every `Cookie` header of the request.
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> requestCookies(const http::Request& request) {
    std::vector<std::pair<std::string, std::string>> cookies;
    for (const auto& line : request.getHeadersValues("Cookie")) {
        std::string_view rest = line;
        while (!rest.empty()) {
            const auto end  = rest.find(';');
            const auto pair = trimBlanks(rest.substr(0, end));
            if (const auto equals = pair.find('='); equals != std::string_view::npos && equals > 0)
                cookies.emplace_back(std::string(pair.substr(0, equals)), std::string(pair.substr(equals + 1)));
            if (end == std::string_view::npos)
                break;
            rest.remove_prefix(end + 1);
        }
    }
    return cookies;
}

/// How a read response is presented to the browser.
struct ReadPresentation {
    std::string contentType;  ///< What is announced: an active type is announced as application/octet-stream.
    bool        inlineOk;     ///< False: the response is served with Content-Disposition: attachment.
};

/// The closed list of passive types shown inline (raster images, audio, video, text/plain); every other type is a download.
/// HTML, XML, JavaScript and WebAssembly are announced as application/octet-stream; SVG keeps its type, for `<img>`.
[[nodiscard]] inline ReadPresentation presentRead(std::string_view contentType) {
    const auto  announced = std::string(trimBlanks(contentType));
    const auto  semicolon = announced.find(';');
    const auto  essence   = lowerAscii(trimBlanks(std::string_view{announced}.substr(0, semicolon)));
    if (essence.empty())
        return {"application/octet-stream", false};
    static constexpr std::string_view active[] = {"text/html",       "application/xhtml+xml",   "text/xml",         "application/xml",
                                                  "text/javascript", "application/javascript",  "application/x-javascript",
                                                  "text/ecmascript", "application/ecmascript",  "application/wasm"};
    // Every spelling a browser runs as a script (text/javascript1.5, application/x-ecmascript, text/jscript, ...), and
    // stylesheets, are announced as opaque bytes.
    const bool scripting = essence.contains("javascript") || essence.contains("ecmascript") || essence.contains("jscript")
                           || essence.contains("livescript") || essence.contains("vbscript") || essence == "text/css";
    const bool isActive = scripting || std::ranges::find(active, essence) != std::end(active)
                          || (essence.ends_with("+xml") && essence != "image/svg+xml") || essence.ends_with("+wasm");
    if (isActive)
        return {"application/octet-stream", false};
    static constexpr std::string_view raster[] = {"image/png", "image/jpeg", "image/gif",  "image/webp",
                                                  "image/avif", "image/bmp", "image/apng", "image/x-icon", "image/vnd.microsoft.icon"};
    const bool passive = std::ranges::find(raster, essence) != std::end(raster) || essence.starts_with("audio/") || essence.starts_with("video/")
                         || essence == "text/plain";
    return {announced, passive};
}

/// A file name safe inside a quoted `filename` parameter (printable ASCII, no quote, backslash or separator).
[[nodiscard]] inline std::string attachmentName(std::string_view rest) {
    const auto  slash = rest.find_last_of('/');
    std::string name(slash == std::string_view::npos ? rest : rest.substr(slash + 1));
    std::string ascii, encoded;
    for (const char raw : name) {
        const auto c = static_cast<unsigned char>(raw);
        const bool plain = c >= 0x20 && c < 0x7f && c != '"' && c != '\\' && c != ';' && c != '%' && c != '/';
        ascii += plain ? static_cast<char>(c) : '_';
        if (std::isalnum(c) || std::string_view{"!#$&+-.^_`|~"}.contains(static_cast<char>(c))) {
            encoded += static_cast<char>(c);
        } else {
            constexpr std::string_view digits = "0123456789ABCDEF";
            encoded += '%';
            encoded += digits[c >> 4];
            encoded += digits[c & 15];
        }
    }
    if (ascii.empty())
        return "attachment; filename=\"download\"";
    return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

inline void setHeader(http::RouteReply& reply, std::string_view name, std::string value) {
    std::erase_if(reply.headers, [&](const auto& header) { return lowerAscii(header.first) == lowerAscii(name); });
    reply.headers.emplace_back(std::string(name), std::move(value));
}

/// Applies to a read response the headers that neutralize active content and keep the token out of caches and referrers.
inline void hardenRead(http::RouteReply& reply, std::string_view rest) {
    std::string contentType;
    for (const auto& [name, value] : reply.headers)
        if (lowerAscii(name) == "content-type")
            contentType = value;
    const auto presentation = presentRead(contentType);
    setHeader(reply, "Content-Type", presentation.contentType);
    // A handler cannot choose a presentation of its own: the disposition is derived from the announced type.
    std::erase_if(reply.headers, [](const auto& header) { return lowerAscii(header.first) == "content-disposition"; });
    if (!presentation.inlineOk)
        reply.headers.emplace_back("Content-Disposition", attachmentName(rest));
    setHeader(reply, "Cache-Control", "no-store");
    setHeader(reply, "Referrer-Policy", "no-referrer");
    setHeader(reply, "Content-Security-Policy", "sandbox; default-src 'none'");
    setHeader(reply, "X-Content-Type-Options", "nosniff");
    setHeader(reply, "Cross-Origin-Resource-Policy", "same-origin");
}

}  // namespace detail

/// The clients, connections and read grants of one application. Thread-safe: its routes run on worker threads.
class ReadBinding {
public:
    using ConnectionKey = std::uint64_t;
    using Clock         = std::function<std::chrono::steady_clock::time_point()>;

    /// The URL space whose cookie path is `servicesPath`: read routes live under it, the exchange under `bindPath`.
    static constexpr std::string_view servicesPath = "/webfront/services/";
    static constexpr std::string_view bindPath     = "/webfront/services/bind/";
    static constexpr std::string_view cookiePrefix = "wf-client-";
    static constexpr std::size_t      tokenIdSize = 16, tokenSecretSize = 64;
    /// The longest lifetime of a transferable grant, which is a bearer capability.
    static constexpr std::chrono::seconds maxTransferableLifetime{15 * 60};

    explicit ReadBinding(RemoteFileTransport declared = RemoteFileTransport::refused, Clock clock = [] { return std::chrono::steady_clock::now(); })
        : transport(declared), now(std::move(clock)), ownPrefix(std::string(cookiePrefix) + automation::channel::nonce().substr(0, 8) + "-") {}

    /// Names the cookies after `name` (the port the server listens on) instead of a random word, so that a restart on the
    /// same port erases the cookies of its previous run. Call before the first `open`.
    void useNamespace(std::string_view name) {
        std::scoped_lock lock{mutex};
        ownPrefix = std::string(cookiePrefix) + std::string(name) + "-";
    }

    /// The cookie name prefix of this application: cookies are shared across ports of a host, so each binding names (and
    /// erases) only its own, `wf-client-<application>-<client>`.
    [[nodiscard]] const std::string& clientCookiePrefix() const noexcept { return ownPrefix; }

    ReadBinding(const ReadBinding&)            = delete;
    ReadBinding& operator=(const ReadBinding&) = delete;

    [[nodiscard]] RemoteFileTransport declaredTransport() const noexcept { return transport; }

    /// A connection starts. `local` is true only for the view of an embedded frontend. Returns the one-time secret
    /// that the page exchanges for its cookie.
    [[nodiscard]] std::string open(ConnectionKey key, bool local) {
        auto             secret = automation::channel::nonce();
        std::scoped_lock lock{mutex};
        if (stopped)
            return secret;  // nothing is registered: the exchange of this secret finds no connection
        connections[key] = Connection{.local = local, .client = {}, .exchange = secret};
        exchanges[secret] = key;
        return secret;
    }

    /// The application stops: every connection, client and grant is dropped, and nothing is granted afterwards.
    void shutdown() {
        Released         released;  // destroyed after the lock: a context's destructor may call back into the binding
        std::scoped_lock lock{mutex};
        stopped = true;
        for (auto& [id, grant] : grants)
            released.push_back(std::move(grant.context));
        grants.clear();
        connections.clear();
        exchanges.clear();
        clients.clear();
    }

    /// A connection ended: its grants die, its client is forgotten with its last connection.
    void close(ConnectionKey key) {
        Released         released;
        std::scoped_lock lock{mutex};
        const auto       found = connections.find(key);
        if (found == connections.end())
            return;
        exchanges.erase(found->second.exchange);
        if (const auto client = clients.find(found->second.client); client != clients.end()) {
            client->second.connections.erase(key);
            if (client->second.connections.empty())
                clients.erase(client);
        }
        connections.erase(found);
        detach([key](const auto& grant) { return grant.second.owner == key; }, released);
    }

    /// Gives a read URL to the connection `key`: `prefix` is the route's, `rest` the path under the token (it is
    /// percent-encoded here). Throws ReadRefused when the connection is gone, its transport is refused, or the options
    /// are unacceptable.
    [[nodiscard]] std::string grant(ConnectionKey key, std::string_view routeId, std::string_view prefix, std::string_view rest, ReadOptions options) {
        if (options.lifetime.count() < 0)
            throw ReadRefused("A read lifetime cannot be negative");
        if (options.transferable && (options.lifetime.count() <= 0 || options.lifetime > maxTransferableLifetime))
            throw ReadRefused("A transferable read needs a lifetime of at most 15 minutes");
        // The server refuses a target holding ".." or a control character before any route sees it: such a URL would never work.
        if (rest.contains("..") || std::ranges::any_of(rest, [](char c) { return static_cast<unsigned char>(c) < 32 || c == 127; }))
            throw ReadRefused("The path of a read cannot contain \"..\" or control characters");
        if (http::routing::collapseSegments("/" + std::string(rest)) != "/" + std::string(rest))
            throw ReadRefused("The path of a read cannot hold empty or dot segments");
        std::string      id = automation::channel::nonce().substr(0, 16), secret = automation::channel::nonce();
        Released         released;
        std::scoped_lock lock{mutex};
        reapExpired(released);
        const auto       connection = connections.find(key);
        if (stopped || connection == connections.end())
            throw ReadRefused("The connection is closed");
        if (!connection->second.local && transport == RemoteFileTransport::refused)
            throw ReadRefused("This connection is not the view of an embedded frontend: declare WebFrontConfig::remoteFileTransport to read files");
        Grant record{.owner = key, .route = std::string(routeId), .secret = secret, .path = std::string(rest), .transferable = options.transferable, .expiry = {}, .context = std::make_shared<const std::any>(std::move(options.context))};
        if (options.lifetime.count() > 0)
            record.expiry = now() + options.lifetime;
        grants[id] = std::move(record);
        return std::string(prefix) + id + "." + secret + "/" + encodePath(rest);
    }

    /// Revokes the grant of a URL or token; unknown ones are ignored.
    void revoke(std::string_view urlOrToken) {
        // A URL is prefix + token + "/" + path; the token is the segment of the form <16 hex>.<64 hex>.
        std::string_view token;
        for (std::size_t from = 0; from < urlOrToken.size();) {
            const auto end     = urlOrToken.find('/', from);
            const auto segment = urlOrToken.substr(from, end == std::string_view::npos ? std::string_view::npos : end - from);
            if (segment.size() == tokenIdSize + 1 + tokenSecretSize && segment[tokenIdSize] == '.') {
                token = segment;
                break;
            }
            if (end == std::string_view::npos)
                break;
            from = end + 1;
        }
        if (token.empty())
            return;
        Released         released;
        std::scoped_lock lock{mutex};
        if (const auto found = grants.find(std::string(token.substr(0, tokenIdSize)));
            found != grants.end() && detail::viewSecretMatches(found->second.secret, token.substr(tokenIdSize + 1))) {
            released.push_back(std::move(found->second.context));
            grants.erase(found);
        }
    }

    /// The handler of the exchange route (`bindPath`): `<secret>` swaps the one-time secret for a cookie, `check`
    /// answers 200 only to a request that carries a cookie of a known client.
    [[nodiscard]] http::RouteHandler exchangeHandler() {
        return [this](const http::RouteRequest& request) {
            auto reply = plain(http::Response::ok);
            detail::setHeader(reply, "Cache-Control", "no-store");
            detail::setHeader(reply, "Referrer-Policy", "no-referrer");
            const auto what = request.path.substr(bindPath.size());
            const auto cookies = detail::requestCookies(request.request);
            if (what == "check") {
                std::scoped_lock lock{mutex};
                if (cookieClient(cookies).empty())
                    reply.status = http::Response::forbidden;
                return reply;
            }
            if (!exchange(std::string(what), cookies, reply))
                reply.status = http::Response::forbidden;
            return reply;
        };
    }

    /// Wraps `handler` as a route handler that serves it only for a request which presents an acceptable token (and cookie).
    /// The prefix this is registered under must be `prefix`, which ends with '/'.
    [[nodiscard]] http::RouteHandler readHandler(std::string routeId, std::string prefix, ReadHandler handler) {
        return [this, routeId = std::move(routeId), prefix = std::move(prefix), handler = std::move(handler)](const http::RouteRequest& request) {
            const auto after = request.path.substr(std::min(prefix.size(), request.path.size()));
            const auto slash = after.find('/');
            const auto token = after.substr(0, slash);
            const auto rest  = slash == std::string_view::npos ? std::string_view{} : after.substr(slash + 1);
            auto       context = authorize(routeId, token, rest, detail::requestCookies(request.request));
            if (!context) {
                auto denied = plain(http::Response::forbidden);
                detail::hardenRead(denied, {});
                return denied;
            }
            auto reply = handler(request, ReadGrant{rest, **context});
            detail::hardenRead(reply, rest);
            return reply;
        };
    }

    /// True when `token` is accepted for a request that presents `cookies`: used by the route, exposed for tests.
    [[nodiscard]] bool accepts(std::string_view routeId, std::string_view token, std::string_view rest, const std::vector<std::pair<std::string, std::string>>& cookies) {
        return authorize(routeId, token, rest, cookies).has_value();
    }

private:
    using Released = std::vector<std::shared_ptr<const std::any>>;

    struct Connection {
        bool        local = false;
        std::string client;    // empty until the exchange
        std::string exchange;  // the one-time secret, until used
    };
    struct Client {
        std::string           secret;
        std::set<ConnectionKey> connections;
    };
    struct Grant {
        ConnectionKey                                        owner = 0;
        std::string                                          route;  // the registration that issued it
        std::string                                          secret;
        std::string                                          path;  // the path it was issued for
        bool                                                 transferable = false;
        std::optional<std::chrono::steady_clock::time_point> expiry;
        std::shared_ptr<const std::any>                      context;
    };

    static http::RouteReply plain(http::Response::StatusCode status) {
        http::RouteReply reply;
        reply.status        = status;
        reply.contentLength = 0;
        return reply;
    }

    static std::string encodePath(std::string_view path) {
        std::string encoded;
        for (const char raw : path) {
            const auto c = static_cast<unsigned char>(raw);
            if (std::isalnum(c) || c == '/' || std::string_view{"-._~"}.contains(static_cast<char>(c))) {
                encoded += static_cast<char>(c);
            } else {
                constexpr std::string_view digits = "0123456789ABCDEF";
                encoded += '%';
                encoded += digits[c >> 4];
                encoded += digits[c & 15];
            }
        }
        return encoded;
    }

    /// Drops the expired grants and their contexts, whether or not their tokens are ever presented again. Caller holds the lock.
    void reapExpired(Released& released) {
        const auto instant = now();
        detach([instant](const auto& grant) { return grant.second.expiry && instant >= *grant.second.expiry; }, released);
    }

    /// Removes the grants matching `selected`, handing their contexts to `released` so that the caller destroys them once
    /// the lock is dropped. Caller holds the lock.
    template <typename Predicate>
    void detach(Predicate selected, Released& released) {
        for (auto grant = grants.begin(); grant != grants.end();) {
            if (selected(*grant)) {
                released.push_back(std::move(grant->second.context));
                grant = grants.erase(grant);
            } else {
                ++grant;
            }
        }
    }

    /// The id of the first client whose cookie is presented with its right secret; empty if none. Caller holds the lock.
    std::string cookieClient(const std::vector<std::pair<std::string, std::string>>& cookies) const {
        for (const auto& [name, value] : cookies) {
            if (!name.starts_with(ownPrefix))
                continue;
            const auto id = name.substr(ownPrefix.size());
            if (const auto client = clients.find(id); client != clients.end() && detail::viewSecretMatches(client->second.secret, value))
                return id;
        }
        return {};
    }

    /// True when one of the cookies carries the right secret of client `id`. Caller holds the lock.
    bool cookieDesignates(const std::vector<std::pair<std::string, std::string>>& cookies, const std::string& id) const {
        if (id.empty())
            return false;
        const auto client = clients.find(id);
        if (client == clients.end())
            return false;
        bool designated = false;
        for (const auto& [name, value] : cookies)
            if (name.size() == ownPrefix.size() + id.size() && name.starts_with(ownPrefix) && name.ends_with(id))
                designated = detail::viewSecretMatches(client->second.secret, value) || designated;
        return designated;
    }

    bool exchange(const std::string& secret, const std::vector<std::pair<std::string, std::string>>& cookies, http::RouteReply& reply) {
        std::scoped_lock lock{mutex};
        const auto       pending = exchanges.find(secret);
        if (pending == exchanges.end())
            return false;
        const auto key = pending->second;
        exchanges.erase(pending);
        const auto connection = connections.find(key);
        if (connection == connections.end())
            return false;
        connection->second.exchange.clear();
        // Cookies named after a client the server forgot are erased; the ones of a known client are left alone.
        for (const auto& [name, value] : cookies)
            if (name.starts_with(ownPrefix) && !clients.contains(name.substr(ownPrefix.size())))
                reply.headers.emplace_back("Set-Cookie", name + "=; Max-Age=0; Path=" + std::string(servicesPath) + "; HttpOnly; SameSite=Strict");
        auto id = cookieClient(cookies);
        if (id.empty()) {
            id = automation::channel::nonce().substr(0, 16);
            Client created{.secret = automation::channel::nonce(), .connections = {}};
            std::string cookie = ownPrefix + id + "=" + created.secret + "; Path=" + std::string(servicesPath) + "; HttpOnly; SameSite=Strict";
            if (transport == RemoteFileTransport::encryptedByDeployment)
                cookie += "; Secure";
            reply.headers.emplace_back("Set-Cookie", std::move(cookie));
            clients.emplace(id, std::move(created));
        }
        clients[id].connections.insert(key);
        connection->second.client = id;
        return true;
    }

    /// The context of the grant, when `token` and the cookies are acceptable. Every refusal looks alike.
    std::optional<std::shared_ptr<const std::any>> authorize(std::string_view routeId, std::string_view token, std::string_view rest, const std::vector<std::pair<std::string, std::string>>& cookies) {
        const auto dot = token.find('.');
        if (dot == std::string_view::npos)
            return std::nullopt;
        Released         released;
        std::scoped_lock lock{mutex};
        reapExpired(released);
        const auto       found = grants.find(std::string(token.substr(0, dot)));
        if (found == grants.end() || !detail::viewSecretMatches(found->second.secret, token.substr(dot + 1)))
            return std::nullopt;
        const auto& grant = found->second;
        if (grant.path != rest)  // a token opens the path it was issued for, nothing else under the route
            return std::nullopt;
        if (grant.route != routeId)  // a grant is valid for the registration that issued it, not for a later route on the prefix
            return std::nullopt;
        if (grant.expiry && now() >= *grant.expiry) {
            released.push_back(std::move(found->second.context));
            grants.erase(found);
            return std::nullopt;
        }
        const auto connection = connections.find(grant.owner);
        if (connection == connections.end())
            return std::nullopt;
        if (!connection->second.local && transport == RemoteFileTransport::refused)
            return std::nullopt;
        if (!grant.transferable) {
            // Any one of the presented cookies may designate the owner's client (concurrent exchanges leave several).
            if (!cookieDesignates(cookies, connection->second.client))
                return std::nullopt;
        }
        return grant.context;
    }

    RemoteFileTransport                transport;
    Clock                              now;
    std::string                        ownPrefix;
    std::mutex                         mutex;
    std::map<ConnectionKey, Connection> connections;
    std::map<std::string, ConnectionKey> exchanges;
    std::map<std::string, Client>      clients;
    std::map<std::string, Grant>       grants;
    bool                               stopped = false;
};

/// A registered read route (BasicWF::addReadRoute): the route lasts as long as this object. Move-only.
class ReadRoute {
public:
    ReadRoute() = default;
    [[nodiscard]] const std::string& prefix() const noexcept { return prefixValue; }
    [[nodiscard]] const std::string& id() const noexcept { return idValue; }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(registration); }

    ReadRoute(std::string id, std::string prefix, std::shared_ptr<ReadBinding> owner, http::RouteTable::Registration route)
        : idValue(std::move(id)), prefixValue(std::move(prefix)), bound(std::move(owner)), registration(std::move(route)) {}
    [[nodiscard]] const std::shared_ptr<ReadBinding>& binding() const {
        if (!bound)
            throw ReadRefused("The read route is empty");
        return bound;
    }

private:
    std::string                    idValue;
    std::string                    prefixValue;
    std::shared_ptr<ReadBinding>   bound;
    http::RouteTable::Registration registration;
};

}  // namespace webfront
