// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-22
//
// WebFront — automation
// Role: MCP over Streamable HTTP, served by the application itself on its own loopback listener (ADR-025).
// Thread: worker-safe | Alloc: unrestricted
// LLM: security sensitive; every request is screened on headers before its body is read

// MCP over Streamable HTTP, served by the application itself on its own loopback listener (ADR-025).
//
// `mcphttp::Server` listens on 127.0.0.1 only, on a port of its own (never the page's), and answers one path, `/mcp`,
// with the limited MCP profile of `Mcp.hpp`: JSON responses only, no server stream. Every request is screened on its
// headers before its body is read: exact `Host` (DNS rebinding), no `Origin` nor `Sec-Fetch-*` (web pages), then a
// session Bearer token compared in constant time, then the media types and the announced length. The token is written
// with the URL in `mcp.json`, a private client configuration in the instance's private directory, never in the project.
// Admission bounds the peers that have not authenticated: a bounded queue of unauthenticated peers with a two-second deadline,
// and separate workers for the authenticated requests.

#pragma once
#if defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION
    #error "WebFront automation does not exist in a distribution build (WEBFRONT_DISTRIBUTION, ADR-025)"
#endif
#include "../http/HTTPServer.hpp"
#include "Channel.hpp"
#include "Mcp.hpp"

#include <algorithm>
#include <cctype>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::automation::mcphttp {
/// The MCP revision of this profile, checked in `MCP-Protocol-Version` once a session is initialized.
inline constexpr std::string_view protocolVersion = "2025-11-25";
/// The request line and the headers, together.
inline constexpr std::size_t headerLimit = 8192;
/// The MCP sessions one listener keeps; the least recently used is evicted beyond.
inline constexpr std::size_t maxSessions = 16;
/// The name of the client configuration file in the private directory.
inline constexpr const char* configurationFile = "mcp.json";

/// An HTTP answer; the server adds `Content-Length` and `Connection: close` (one request per connection).
struct Answer {
    int                                              status = 200;
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string                                      body{};

    static std::string_view reason(int status) {
        switch (status) {
            case 100:
                return "Continue";
            case 200:
                return "OK";
            case 202:
                return "Accepted";
            case 204:
                return "No Content";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 406:
                return "Not Acceptable";
            case 411:
                return "Length Required";
            case 413:
                return "Content Too Large";
            case 415:
                return "Unsupported Media Type";
            case 421:
                return "Misdirected Request";
            case 431:
                return "Request Header Fields Too Large";
            case 501:
                return "Not Implemented";
            case 503:
                return "Service Unavailable";
            default:
                return "Internal Server Error";
        }
    }
    std::string serialize() const {
        std::string text = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason(status)) + "\r\n";
        for (const auto& [name, value] : headers)
            text += name + ": " + value + "\r\n";
        if (!body.empty())
            text += "Content-Type: application/json\r\n";
        // HTTP forbids Content-Length on a 204 answer, even a zero one (RFC 9110, 8.6).
        if (status != 204)
            text += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        text += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
        return text + body;
    }
};

/// A refusal of the transport, before any MCP message: a short JSON body that quotes nothing of the request.
inline Answer refusal(int status, std::string_view message) {
    return {.status = status, .headers = {}, .body = "{\"error\":" + json::quote(message) + "}"};
}

namespace detail {
inline std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return std::string(text);
}
inline std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}
/// The single value of a header; empty when it is missing, nullopt when it is repeated.
inline std::optional<std::string> single(const http::Request& request, std::string_view name) {
    const auto values = request.getHeadersValues(name);
    if (values.size() > 1)
        return std::nullopt;
    return values.empty() ? std::string{} : trimmed(values.front());
}
inline bool present(const http::Request& request, std::string_view name) {
    return !request.getHeadersValues(name).empty();
}
/// Whether the `Accept` headers accept `application/json`. As RFC 9110 (12.5.1) requires, the quality that applies is the one
/// of the most specific matching range: `application/json`, then `application/*`, then `*/*`. A quality of zero refuses, so
/// `application/json;q=0, */*` refuses JSON; `application/jsonp` matches nothing.
inline bool acceptsJson(const http::Request& request) {
    int  specificity = 0;  // of the range that applies so far: 3 exact, 2 application/*, 1 */*, 0 none
    bool accepted    = false;
    for (const auto& header : request.getHeadersValues("Accept")) {
        std::string_view rest{header};
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const auto item  = rest.substr(0, comma);
            rest             = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const auto semi  = item.find(';');
            const auto type  = lower(trimmed(item.substr(0, semi)));
            const int  level = type == "application/json" ? 3 : type == "application/*" ? 2 : type == "*/*" ? 1 : 0;
            if (level == 0)
                continue;
            bool refused = false;
            for (auto parameters = semi == std::string_view::npos ? std::string_view{} : item.substr(semi + 1); !parameters.empty();) {
                const auto next      = parameters.find(';');
                const auto parameter = trimmed(parameters.substr(0, next));
                parameters           = next == std::string_view::npos ? std::string_view{} : parameters.substr(next + 1);
                const auto equals    = parameter.find('=');
                if (equals == std::string::npos || lower(trimmed(std::string_view(parameter).substr(0, equals))) != "q")
                    continue;
                const auto quality = trimmed(std::string_view(parameter).substr(equals + 1));
                // A quality is 0 to 1 with up to three decimals: any non-zero digit makes it acceptable.
                refused = quality.find_first_of("123456789") == std::string::npos;
            }
            // A more specific range overrides a less specific one; between ranges of the same level, a refusal wins.
            if (level > specificity) {
                specificity = level;
                accepted    = !refused;
            } else if (level == specificity && refused)
                accepted = false;
        }
    }
    return accepted;
}
}  // namespace detail

/// What the headers alone decide: a refusal, or the length of the body to read before the request is handled.
struct Screening {
    std::optional<Answer> refusal{};
    std::size_t           length          = 0;
    bool                  expectsContinue = false;
};

/// Decides on the headers of a request, before its body is read: path, `Host`, `Origin` and `Sec-Fetch-*`, method,
/// the Bearer token, then the media types and the announced length of a `POST`. `port` is the listener's own.
inline Screening screen(const http::Request& request, unsigned short port, std::string_view token, bool authentication = true) {
    using detail::lower;
    // DNS rebinding: a name that resolves to 127.0.0.1 still carries its own Host. Only the two loopback spellings pass.
    const auto host  = detail::single(request, "Host");
    const auto local = ":" + std::to_string(port);
    if (!host || (lower(*host) != "127.0.0.1" + local && lower(*host) != "localhost" + local))
        return {.refusal = refusal(421, "This listener answers 127.0.0.1 and localhost only")};
    // A browser sends Origin on every POST, and Sec-Fetch-* on every request it makes: no web page may reach the route,
    // the application's own page included. A native MCP client sends neither.
    if (detail::present(request, "Origin") || std::ranges::any_of(request.headers, [](const auto& header) {
            return detail::lower(header.name).starts_with("sec-fetch-");
        }))
        return {.refusal = refusal(403, "Requests from web pages are refused")};
    if (request.uri != "/mcp")
        return {.refusal = refusal(404, "Not found")};
    using Method = http::Request::Method;
    if (request.method != Method::Post && request.method != Method::Delete) {
        // GET has no server stream in this profile; OPTIONS (a CORS preflight) gets no Access-Control-* header either.
        auto answer = refusal(405, "This MCP endpoint accepts POST and DELETE");
        answer.headers.emplace_back("Allow", "POST, DELETE");
        return {.refusal = std::move(answer)};
    }
    const auto authorization = detail::single(request, "Authorization");
    bool       authorized    = !authentication;
    if (authentication && authorization && !authorization->empty()) {
        const auto space = authorization->find(' ');
        if (space != std::string::npos && lower(authorization->substr(0, space)) == "bearer")
            authorized = channel::sameToken(detail::trimmed(std::string_view(*authorization).substr(space + 1)), token);
    }
    if (!authorized) {
        auto answer = refusal(401, "A valid Bearer token is required");
        answer.headers.emplace_back("WWW-Authenticate", "Bearer");
        return {.refusal = std::move(answer)};
    }
    if (detail::present(request, "Transfer-Encoding"))
        return {.refusal = refusal(501, "Chunked bodies are not supported")};
    const auto length = http::routing::contentLength(request);
    if (length == http::routing::malformedLength)
        return {.refusal = refusal(400, "Malformed Content-Length")};
    if (request.method == Method::Delete) {
        if (length && *length != 0)
            return {.refusal = refusal(400, "DELETE has no body")};
        return {};
    }
    const auto type = detail::single(request, "Content-Type");
    if (!type || lower(detail::trimmed(std::string_view(*type).substr(0, type->find(';')))) != "application/json")
        return {.refusal = refusal(415, "The body must be application/json")};
    if (!detail::acceptsJson(request))
        return {.refusal = refusal(406, "Accept must include application/json")};
    if (!length)
        return {.refusal = refusal(411, "Content-Length is required")};
    if (*length > messageLimit)
        return {.refusal = refusal(413, "The body exceeds " + std::to_string(messageLimit) + " bytes")};
    return {.refusal = std::nullopt, .length = static_cast<std::size_t>(*length), .expectsContinue = http::routing::expectsContinue(request)};
}

/// The MCP sessions of a listener and the handling of a screened request. Thread-safe: the workers share it.
template <typename Backend>
class Endpoint {
public:
    explicit Endpoint(Backend& controller) : backend(controller) {}

    /// Handles a request that `screen` accepted, with its whole body.
    Answer handle(const http::Request& request, std::string_view body) {
        const auto identifier = detail::single(request, "Mcp-Session-Id");
        if (!identifier)
            return refusal(400, "Mcp-Session-Id is repeated");
        // Every message of an initialized session, its termination included, names a supported revision (or none, which
        // MCP lets the server read as its default); a rejected one has no effect on the session.
        const auto negotiated       = detail::single(request, "MCP-Protocol-Version");
        const bool supportedVersion = negotiated && (negotiated->empty() || *negotiated == protocolVersion);
        const auto unsupported      = [] {
            return rpcError(400, -32600, "Unsupported MCP-Protocol-Version; this server supports " + std::string(protocolVersion));
        };
        if (request.method == http::Request::Method::Delete) {
            if (identifier->empty())
                return refusal(400, "DELETE needs Mcp-Session-Id");
            if (!known(*identifier))
                return refusal(404, "Unknown or terminated MCP session");
            if (!supportedVersion)
                return unsupported();
            const std::scoped_lock lock{mutex};
            if (conversations.erase(*identifier) == 0)
                return refusal(404, "Unknown or terminated MCP session");
            return {.status = 204};
        }
        const auto start = body.find_first_not_of(" \t\r\n");
        if (start != std::string_view::npos && body[start] == '[')
            return rpcError(400, -32600, "Batches are not supported");
        std::string method;
        bool        isRequest = false;
        try {
            const Object message{body};
            method    = message.has("method") ? message.string("method") : std::string{};
            isRequest = message.has("id") && message.has("method");
            if (!message.has("method")) {
                // A response of the client: this server sends no request, so there is nothing to route it to. It is
                // still checked: a JSON-RPC 2.0 response with an id and exactly one of result and error.
                if (message.string("jsonrpc") != "2.0" || !message.has("id") || message.has("result") == message.has("error"))
                    return rpcError(400, -32600, "Expected a JSON-RPC 2.0 request, notification or response");
                if (identifier->empty())
                    return rpcError(400, -32600, "Missing Mcp-Session-Id");
                if (!known(*identifier))
                    return refusal(404, "Unknown or terminated MCP session");
                return supportedVersion ? Answer{.status = 202} : unsupported();
            }
        } catch (const std::exception& failure) {
            return rpcError(400, -32700, failure.what());
        }
        if (identifier->empty()) {
            if (method != "initialize" || !isRequest)
                return rpcError(400, -32600, "Missing Mcp-Session-Id: initialize first");
            auto conversation = std::make_shared<Conversation>(backend);
            auto response     = conversation->mcp.receive(body);
            if (!response)
                return rpcError(400, -32600, "initialize is a request");
            Answer answer{.status = 200, .headers = {}, .body = std::move(*response)};
            // The answer quotes the request's id and may exceed the request limit: success is read from the profile.
            if (conversation->mcp.isInitialized()) {
                const auto created = channel::nonce().substr(0, 32);
                answer.headers.emplace_back("Mcp-Session-Id", created);
                const std::scoped_lock lock{mutex};
                if (conversations.size() >= maxSessions) {
                    const auto oldest = std::ranges::min_element(conversations, {}, [](const auto& item) {
                        return item.second->used;
                    });
                    conversations.erase(oldest);
                }
                conversations.emplace(created, std::move(conversation));
            }
            return answer;
        }
        std::shared_ptr<Conversation> conversation;
        {
            const std::scoped_lock lock{mutex};
            const auto             found = conversations.find(*identifier);
            if (found == conversations.end())
                return rpcError(404, -32600, "Unknown or terminated MCP session");
            if (!supportedVersion)
                return unsupported();
            conversation = found->second;
        }
        const std::scoped_lock lock{conversation->mutex};
        auto                   response = conversation->mcp.receive(body);
        // Only an accepted message counts as activity for the eviction of the least recently used session: a supported
        // revision and a valid JSON-RPC envelope. `used` is only ever written and read under the registry's lock.
        if (conversation->mcp.lastEnvelopeAccepted()) {
            const std::scoped_lock registry{mutex};
            conversation->used = std::chrono::steady_clock::now();
        }
        if (!response)
            return {.status = 202};
        return {.status = 200, .headers = {}, .body = std::move(*response)};
    }
    /// The number of live MCP sessions.
    std::size_t sessions() const {
        const std::scoped_lock lock{mutex};
        return conversations.size();
    }

private:
    struct Conversation {
        explicit Conversation(Backend& backend) : mcp(backend) {}
        std::mutex                            mutex;
        Mcp<Backend>                          mcp;
        std::chrono::steady_clock::time_point used = std::chrono::steady_clock::now();
    };
    Backend&                                             backend;
    mutable std::mutex                                   mutex;
    std::map<std::string, std::shared_ptr<Conversation>> conversations;

    bool known(const std::string& identifier) const {
        const std::scoped_lock lock{mutex};
        return conversations.contains(identifier);
    }
    static Answer rpcError(int status, int code, std::string_view message) {
        return {.status  = status,
                .headers = {},
                .body    = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + json::quote(message) + "}}"};
    }
};

/// The client configuration of an instance: its URL and its token. `name` names the server for the client.
inline std::string clientConfiguration(std::string_view url, std::string_view token, std::string_view name = "webfront", bool authentication = true) {
    std::string server = "{\"type\":" + json::quote("http") + ",\"url\":" + json::quote(url);
    if (authentication)
        server += ",\"headers\":"
                  + objectJson({
                      {"Authorization", json::quote("Bearer " + std::string(token))}
        });
    server += "}";
    return "{\n  \"mcpServers\": {\n    " + json::quote(name) + ": " + server + "\n  }\n}\n";
}

/// Validate options at each independent entry point: enableAutomation, direct Automation construction and direct Server
/// construction. The outer checks precede allocation/session creation; Server must also protect its own callers.
inline void validateTokenOptions(const std::optional<std::string>& fixed, bool authentication) {
    if (!authentication && fixed)
        throw std::invalid_argument("A fixed MCP token cannot be combined with disabled authentication");
    if (fixed && (fixed->size() < 32 || fixed->size() > 1024 || !std::ranges::all_of(*fixed, [](unsigned char c) {
                      return c >= 0x21 && c <= 0x7e;
                  })))
        throw std::invalid_argument("MCP token must contain 32 to 1024 visible ASCII characters without spaces");
}

/// A failure to bind or listen on the requested MCP port; retains the native error code.
class BindError : public std::system_error {
public:
    BindError(std::error_code cause, unsigned short port) : std::system_error(cause, "WebFront cannot listen for MCP on 127.0.0.1:" + std::to_string(port)) {}
    /// Whether the native error reports that the requested port is already in use.
    [[nodiscard]] bool addressInUse() const noexcept {
#ifdef _WIN32
        return code().value() == WSAEADDRINUSE;
#else
        return code().value() == EADDRINUSE;
#endif
    }
};

/// The loopback listener. It writes `mcp.json` into `directory` (an existing private directory outside the project) and
/// removes it when it stops. Closing the session stops it; so does `stop()`.
template <typename Backend = Session>
class Server {
public:
    /// @param port 0 (the default) lets the OS choose; a chosen port is never replaced: when it is taken the constructor throws
    /// @throws std::system_error naming the address, the port and the cause when the listener cannot be bound
    Server(std::shared_ptr<Backend>     controller,
           const std::filesystem::path& directory,
           unsigned short               port           = 0,
           std::optional<std::string>   fixedToken     = std::nullopt,
           bool                         authentication = true)
        : backend(std::move(controller)),
          endpoint(*backend),
          token(selectToken(fixedToken, authentication)),
          listener(channel::createSocket()),
          boundPort(bind(listener, port)),
          address("http://127.0.0.1:" + std::to_string(boundPort) + "/mcp"),
          file(directory / configurationFile),
          authenticate(authentication) {
        channel::nonblocking(listener.value);
        channel::writeEndpoint(file, clientConfiguration(address, token, "webfront", authenticate));
        worker = std::jthread([this](std::stop_token stop) {
            try {
                serve(stop);
            } catch (const std::exception&) {
                listener.reset();
                withdraw();
                backend->close("MCP listener failed");
            }
        });
    }
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;
    ~Server() {
        stop();
    }
    /// `http://127.0.0.1:PORT/mcp`.
    const std::string& url() const {
        return address;
    }
    unsigned short port() const {
        return boundPort;
    }
    /// The private client configuration (`mcp.json`), which holds the token.
    const std::filesystem::path& configuration() const {
        return file;
    }
    /// Closes the listener, waits for the requests in progress and removes `mcp.json`. Idempotent.
    void stop() {
        worker.request_stop();
        if (worker.joinable())
            worker.join();
        withdraw();
    }

private:
    [[maybe_unused]] channel::Runtime runtime;
    std::shared_ptr<Backend>          backend;
    Endpoint<Backend>                 endpoint;
    std::string                       token;
    channel::Socket                   listener;
    unsigned short                    boundPort;
    std::string                       address;
    std::filesystem::path             file;
    bool                              authenticate;
    std::once_flag                    withdrawn;
    std::jthread                      worker;

    static constexpr std::size_t maxConnections = 32, maxPending = 64;
    struct Connection {
        std::atomic<bool> finished{false};
        std::jthread      thread;
    };
    // An unauthenticated peer owns a bounded buffer and a descriptor, never a worker. Its headers, then its body, must
    // arrive within two seconds of its connection; a refused peer is drained until then so that the client reads its
    // answer instead of a reset.
    struct Pending {
        channel::Socket                       socket;
        http::Request                         request{};
        std::size_t                           headerBytes = 0;
        std::optional<Screening>              screening{};
        std::string                           body{};
        bool                                  refused  = false;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        explicit Pending(channel::Handle handle) : socket(handle) {
            channel::nonblocking(handle);
        }
    };

    static std::string selectToken(const std::optional<std::string>& fixed, bool authentication) {
        validateTokenOptions(fixed, authentication);
        if (!fixed)
            return authentication ? channel::nonce() : std::string{};
        return *fixed;
    }
    void withdraw() noexcept {
        // A reader on Windows (readEndpoint does not share deletion) delays the removal briefly; it is retried.
        std::call_once(withdrawn, [this] {
            channel::removeWithRetry(file);
            channel::removeWithRetry(file.string() + ".new");
        });
    }
    static void endWrites(channel::Handle socket) noexcept {
#ifdef _WIN32
        ::shutdown(socket, SD_SEND);
#else
        ::shutdown(socket, SHUT_WR);
#endif
    }
    static unsigned short bind(channel::Socket& socket, unsigned short requested) {
#ifdef _WIN32
        const BOOL exclusive = TRUE;
        (void)::setsockopt(socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
#else
        const int reuse = 1;  // rebinding a chosen port after a restart; POSIX still refuses a port in use
        (void)::setsockopt(socket.value, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
        auto address = channel::address(requested);
        if (::bind(socket.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) || ::listen(socket.value, 16)) {
            const std::error_code cause{channel::socketError(), std::system_category()};
            throw BindError(cause, requested);
        }
#ifdef _WIN32
        int size = sizeof address;
#else
        socklen_t size = sizeof address;
#endif
        if (::getsockname(socket.value, reinterpret_cast<sockaddr*>(&address), &size))
            throw std::system_error(std::error_code{channel::socketError(), std::system_category()}, "Cannot discover the MCP port");
        return ntohs(address.sin_port);
    }
    // A refusal fits in the empty send buffer of a fresh peer: never wait on an unauthenticated client.
    static void refuse(Pending& peer, const Answer& answer) {
        (void)channel::SocketWrite{}(peer.socket.value, answer.serialize());
        endWrites(peer.socket.value);
        peer.refused = true;
    }
    // Feeds received bytes to a peer; returns true when its request is complete and accepted.
    bool feed(Pending& peer, std::string_view chunk) {
        if (!peer.screening) {
            try {
                const char* stop  = peer.request.consume(chunk.data(), chunk.data() + chunk.size());
                peer.headerBytes += static_cast<std::size_t>(stop - chunk.data());
                if (peer.headerBytes > headerLimit) {
                    refuse(peer, refusal(431, "The headers exceed " + std::to_string(headerLimit) + " bytes"));
                    return false;
                }
                if (!peer.request.completed())
                    return false;
                chunk.remove_prefix(static_cast<std::size_t>(stop - chunk.data()));
            } catch (const http::BadRequestException&) {
                refuse(peer, refusal(400, "Malformed HTTP request"));
                return false;
            }
            auto screening = screen(peer.request, boundPort, token, authenticate);
            if (screening.refusal) {
                refuse(peer, *screening.refusal);
                return false;
            }
            if (screening.expectsContinue && chunk.size() < screening.length)
                (void)channel::SocketWrite{}(peer.socket.value, "HTTP/1.1 100 Continue\r\n\r\n");
            peer.screening = std::move(screening);
        }
        peer.body.append(chunk.substr(0, std::min(chunk.size(), peer.screening->length - peer.body.size())));
        return peer.body.size() == peer.screening->length;
    }
    void serve(std::stop_token stop) {
        std::list<Connection> connections;
        std::list<Pending>    pending;
        struct Revoke {
            Server& owner;
            ~Revoke() {
                owner.listener.reset();
                owner.withdraw();
            }
        } revoke{*this};
        while (!stop.stop_requested() && !backend->isClosed()) {
            std::erase_if(connections, [](const Connection& connection) {
                return connection.finished.load();
            });
#ifdef _WIN32
            std::vector<WSAPOLLFD> items;
#else
            std::vector<pollfd> items;
#endif
            items.push_back({listener.value, POLLIN, 0});
            for (const auto& peer : pending)
                items.push_back({peer.socket.value, POLLIN, 0});
#ifdef _WIN32
            if (::WSAPoll(items.data(), static_cast<ULONG>(items.size()), 20) < 0)
                continue;
#else
            if (::poll(items.data(), static_cast<nfds_t>(items.size()), 20) < 0)
                continue;
#endif
            std::size_t index = 1;
            for (auto peer = pending.begin(); peer != pending.end(); ++index) {
                bool keep = std::chrono::steady_clock::now() < peer->deadline;
                if (keep && items[index].revents) {
                    char       bytes[4096];
                    const auto count = channel::SocketRead{}(peer->socket.value, bytes, sizeof bytes);
                    if (count > 0 && !peer->refused) {
                        if (feed(*peer, std::string_view(bytes, static_cast<std::size_t>(count)))) {
                            if (connections.size() >= maxConnections)
                                refuse(*peer, refusal(503, "The MCP listener is busy"));
                            else {
                                dispatch(connections, *peer);
                                keep = false;
                            }
                        }
                    } else if (count == 0 || (count < 0 && !channel::retryableSocketError(channel::socketError())))
                        keep = false;
                }
                if (keep)
                    ++peer;
                else
                    peer = pending.erase(peer);
            }
            // Drain a bounded batch, then serve the readable peers before accepting another batch.
            if (items.front().revents)
                for (std::size_t accepted = 0; accepted < maxPending; ++accepted) {
                    const auto handle = channel::acceptSocket(listener.value);
                    if (handle == channel::invalid)
                        break;
                    if (pending.size() >= maxPending)
                        pending.pop_front();
                    pending.emplace_back(handle);
                }
        }
    }
    void dispatch(std::list<Connection>& connections, Pending& peer) {
        auto&           connection = connections.emplace_back();
        channel::Socket transfer{std::exchange(peer.socket.value, channel::invalid)};
        try {
            connection.thread = std::jthread([this, handle = transfer.value, request = std::move(peer.request), body = std::move(peer.body), &connection] {
                respond(handle, request, body);
                connection.finished.store(true);
            });
            transfer.value    = channel::invalid;
        } catch (...) {
            connection.finished.store(true);
            throw;
        }
    }
    void respond(channel::Handle handle, const http::Request& request, const std::string& body) {
        channel::Socket peer{handle};
        Answer          answer;
        try {
            answer = endpoint.handle(request, body);
        } catch (const std::exception&) {
            answer = refusal(500, "The MCP request failed");
        }
        try {
            channel::send(peer.value, answer.serialize(), std::chrono::steady_clock::now() + std::chrono::seconds{30});
            endWrites(peer.value);
            // Let the client read the answer before the descriptor closes, without holding a worker for long.
            char       ignored[512];
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
            while (std::chrono::steady_clock::now() < until) {
                if (!channel::waitUntil(peer.value, POLLIN, until))
                    continue;
                const auto count = channel::SocketRead{}(peer.value, ignored, sizeof ignored);
                if (count == 0 || (count < 0 && !channel::retryableSocketError(channel::socketError())))
                    break;
            }
        } catch (const std::exception&) {}
    }
};
}  // namespace webfront::automation::mcphttp
