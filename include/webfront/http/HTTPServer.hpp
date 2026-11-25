// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-01-16
//
// WebFront — http
// Role: Minimal HTTP server with WebSocket upgrade: connection coroutines awaiting the senders of a network provider.
// Thread: bridge-io | Alloc: unrestricted
// LLM: core transport; connections are coroutines on the io_context thread, owned by a counting_scope

#pragma once
#include "../ApplicationOrigin.hpp"
#include "../ContentPolicy.hpp"
#include "../networking/BasicNetworking.hpp"
#include "../networking/Tasks.hpp"
#include "../system/FileSystem.hpp"
#include "../system/WindowsCompat.hpp"
#include "../Task.hpp"
#include "../tooling/HexDump.hpp"
#include "AcceptEncoding.hpp"
#include "BlockingWorker.hpp"
#include "BuffersPolicy.hpp"
#include "Encodings.hpp"
#include "GzipCache.hpp"
#include "GzipExecutor.hpp"
#include "MimeType.hpp"
#include "Ranges.hpp"
#include "WebSocket.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <list>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace webfront::tls {
class Credentials;
}

namespace webfront::http {

class Headers {
    struct Header {
        Header() = default;
        Header(std::string_view n, std::string_view v) : name(std::move(n)), value(std::move(v)) { }
        std::string name;
        std::string value;
    };

public:
    /// @return the header value with headerName name (or at least the first one)
    [[nodiscard]] std::optional<std::string> getHeaderValue(std::string_view headerName) const {
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName))
                return header.value;
        return {};
    }

    /// @return the list of headers values with the same headerName name
    [[nodiscard]] std::list<std::string> getHeadersValues(std::string_view headerName) const {
        std::list<std::string> values{};
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName))
                values.push_back(header.value);
        return values;
    }

    /// @return true if text is contained in the value field of headerName (case insensitive)
    [[nodiscard]] bool headersContain(std::string_view headerName, std::string_view text) const {
        for (auto header : getHeadersValues(headerName)) {
            if (std::search(header.cbegin(),
                            header.cend(),
                            text.cbegin(),
                            text.cend(),
                            [](char c1, char c2) {
                                return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
                            })
                != header.cend())
                return true;
        }
        return false;
    }

    std::vector<Header> headers;

    /** @brief Matches encoding tokens and q=0 exclusions across all Accept-Encoding fields. */
    [[nodiscard]] bool acceptsEncoding(std::string_view coding) const {
        encoding::Preferences preferences(coding);
        for (const auto& field : getHeadersValues("Accept-Encoding"))
            preferences.add(field);
        return preferences.accepted();
    }

private:
    [[nodiscard]] static constexpr bool caseInsensitiveEqual(std::string_view s1, std::string_view s2) {
        return ((s1.size() == s2.size()) && std::equal(s1.begin(), s1.end(), s2.begin(), [](char c1, char c2) {
                    return (c1 == c2 || std::toupper(static_cast<unsigned char>(c1)) == std::toupper(static_cast<unsigned char>(c2)));
                }));
    }
};

struct BadRequestException : public std::runtime_error {
    BadRequestException() : std::runtime_error("Bad HTTP request") { }
};

struct Request : Headers {
    enum class Method { Connect, Delete, Get, Head, Options, Patch, Post, Put, Trace, Undefined };
    Method      method;
    std::string uri;
    int         httpVersionMajor;
    int         httpVersionMinor;

    void reset() {
        headers.clear();
        uri.clear();
    }

    [[nodiscard]] static Method getMethodFromString(std::string_view text) {
        for (size_t index = 0; index < methodNames.size(); ++index)
            if (methodNames[index] == text)
                return static_cast<Method>(index);
        return Method::Undefined;
    }

    [[nodiscard]] std::string_view getMethodName() const {
        return method == Method::Undefined ? "Undefined" : methodNames[static_cast<size_t>(method)];
    }

    void setMethod(std::string_view text) {
        method = getMethodFromString(text);
    }

    [[nodiscard]] bool isUpgradeRequest(std::string_view protocol) const {
        return headersContain("Connection", "upgrade") && headersContain("Upgrade", protocol);
    }

    template <typename InputIterator>
    bool parseSomeData(InputIterator begin, InputIterator end) {
        while (begin != end)
            if (completeRequest(*begin++))
                return true;

        return false;
    }

    /// Parses up to the end of the headers and returns where it stopped: the bytes after it start the body.
    template <typename InputIterator>
    InputIterator consume(InputIterator begin, InputIterator end) {
        while (begin != end && !completed())
            completeRequest(*begin++);
        return begin;
    }

    bool completed() const {
        return state == State::completed;
    }

private:
    std::string methodBuffer;

    /** @brief Accumulates a validated HTTP version digit without signed overflow. */
    static void appendVersionDigit(int& version, char digit) {
        const auto value = digit - '0';
        if (version > (std::numeric_limits<int>::max() - value) / 10)
            throw BadRequestException();
        version = version * 10 + value;
    }

    // clang-format off
    enum class State { methodStart, method, URI, versionH, versionT1, versionT2, versionP, versionSlash, versionMajorStart, versionMajor, versionMinorStart,
                 versionMinor, newline1, headerLineStart, headerLws, headerName, spaceBeforeHeaderValue, headerValue, newline2, newline3, completed };
    State state { State::methodStart };    
    bool completeRequest(char input) {
        auto isChar = [](char c) { return static_cast<unsigned char>(c) <= 127; };
        auto isCtrl = [](char c) { return static_cast<unsigned char>(c) <= 31 || c == 127; };
        auto isSpecial = [](char c) {   switch (c) {
            case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':': case '\\': case '"':
            case '/': case '[': case ']': case '?': case '=': case '{': case '}': case ' ': case '\t': return true;
            default: return false;
        }};
        auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
        auto setState = [this](bool cond, State next) {
            state = next;
            if (cond) throw BadRequestException();
        };

        using enum State;
        switch (state) {
            case methodStart:
                setState(!isChar(input) || isCtrl(input) || isSpecial(input), method);
                methodBuffer = input;
                break;
            case method: if (input == ' ') { state = URI; setMethod(methodBuffer); }
                              else if (!isChar(input) || isCtrl(input) || isSpecial(input)) { throw BadRequestException(); }
                              else methodBuffer.push_back(input);
                break;
            case URI: if (input == ' ') { state = versionH; break; }
                           else if (isCtrl(input)) throw BadRequestException();
                           else { uri.push_back(input); break; }
            case versionH: setState(input != 'H', versionT1); break;
            case versionT1: setState(input != 'T', versionT2); break;
            case versionT2: setState(input != 'T', versionP); break;
            case versionP: setState(input != 'P', versionSlash); break;
            case versionSlash: setState(input != '/', versionMajorStart); httpVersionMajor = 0; httpVersionMinor = 0; break;
            case versionMajorStart: setState(!isDigit(input), versionMajor); appendVersionDigit(httpVersionMajor, input); break;
            case versionMajor: if (input == '.') state = versionMinorStart;
                                    else if (isDigit(input)) appendVersionDigit(httpVersionMajor, input);
                                    else throw BadRequestException();
                break;
            case versionMinorStart: setState(!isDigit(input), versionMinor); appendVersionDigit(httpVersionMinor, input); break;
            case versionMinor: if (input == '\r') state = newline1;
                                    else if (isDigit(input)) appendVersionDigit(httpVersionMinor, input);
                                    else throw BadRequestException();
                break;
            case newline1: setState(input != '\n', headerLineStart); break;
            case headerLineStart: if (input == '\r') { state = newline3; break; }
                                       else if (!headers.empty() && (input == ' ' || input == '\t')) { state = headerLws; break; }
                                       else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                       else { headers.push_back({}); headers.back().name.push_back(input); state = headerName; break; }
            case headerLws:if (input == '\r') { state = newline2; break; }
                                 else if (input == ' ' || input == '\t') break;
                                 else if (isCtrl(input)) throw BadRequestException();
                                 else { state = headerValue; headers.back().value.push_back(' '); headers.back().value.push_back(input); break; }  // an obsolete line fold becomes one space (RFC 9112 §5.2), not a concatenation
            case headerName: if (input == ':') { state = spaceBeforeHeaderValue; break; }
                                  else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                  else { headers.back().name.push_back(input); break; }
            case spaceBeforeHeaderValue: if (input == ' ' || input == '\t') break;
                                             else if (input == '\r') { state = newline2; break; }
                                             else if (isCtrl(input)) throw BadRequestException();
                                             else { headers.back().value.push_back(input); state = headerValue; break; }
            case headerValue: if (input == '\r') { state = newline2; break; }
                                   else if (isCtrl(input) && input != '\t') throw BadRequestException();
                                   else { headers.back().value.push_back(input); break; }
            case newline2: setState(input != '\n', headerLineStart); break;
            case newline3: if (input == '\n') { state = completed; return true; } else throw BadRequestException();
            case completed: return true;
            default: throw BadRequestException();
        }
        return false;
    }
    inline static std::array methodNames{"CONNECT", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT", "TRACE"};
};  // clang-format on

struct Response : Headers {
    enum StatusCode : uint16_t {
        continue_             = 100,
        switchingProtocols    = 101,
        ok                    = 200,
        accepted              = 202,
        partialContent        = 206,
        movedPermanently      = 301,
        badRequest            = 400,
        unauthorized          = 401,
        forbidden             = 403,
        notFound              = 404,
        methodNotAllowed      = 405,
        notAcceptable         = 406,
        lengthRequired        = 411,
        payloadTooLarge       = 413,
        rangeNotSatisfiable   = 416,
        internalServerError   = 500,
        notImplemented        = 501,
        serviceUnavailable    = 503,
        variantAlsoNegotiates = 506
    };
    StatusCode  statusCode;
    std::string content;

    static Response getStatusResponse(StatusCode code) {
        Response response;
        response.statusCode = code;
        response.content    = "<html><head><title>" + toString(code) + "</title></head>";
        response.content   += "<body><h1>" + std::to_string(code) + " " + toString(code) + "</h1></body></html>";
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", "text/html");

        return response;
    }

    template <typename Net>
    [[nodiscard]] std::vector<typename Net::ConstBuffer> toBuffers() const {
        std::vector<typename Net::ConstBuffer> buffers;
        httpStatus = "HTTP/1.1 " + std::to_string(statusCode) + " " + toString(statusCode) + "\r\n";
        buffers.push_back(Net::Buffer(httpStatus));

        static const char separator[] = {':', ' '};
        static const char crlf[]      = {'\r', '\n'};
        for (auto& header : headers) {
            buffers.push_back(Net::Buffer(header.name));
            buffers.push_back(Net::Buffer(separator));
            buffers.push_back(Net::Buffer(header.value));
            buffers.push_back(Net::Buffer(crlf));
        }
        // This server handles one HTTP request per socket. Prevent a browser from
        // reusing it before shutdown is observed; upgraded WebSockets remain open.
        if (statusCode != switchingProtocols)
            buffers.push_back(Net::Buffer(std::string_view{"Connection: close\r\n"}));
        buffers.push_back(Net::Buffer(crlf));
        if (!content.empty())
            buffers.push_back(Net::Buffer(content));

        return buffers;
    }

private:
    // Buffer views must stay valid independently until this response's asynchronous send ends.
    mutable std::string httpStatus;

    [[nodiscard]] static std::string toString(StatusCode code) {
        static constexpr std::array<std::pair<StatusCode, std::string_view>, 19> names{
            {{continue_, "Continue"},
             {switchingProtocols, "Switching Protocols"},
             {ok, "OK"},
             {accepted, "Accepted"},
             {partialContent, "Partial Content"},
             {movedPermanently, "Moved Permanently"},
             {badRequest, "Bad Request"},
             {unauthorized, "Unauthorized"},
             {forbidden, "Forbidden"},
             {notFound, "Not Found"},
             {methodNotAllowed, "Method Not Allowed"},
             {notAcceptable, "Not Acceptable"},
             {lengthRequired, "Length Required"},
             {payloadTooLarge, "Payload Too Large"},
             {rangeNotSatisfiable, "Range Not Satisfiable"},
             {internalServerError, "Internal Server Error"},
             {notImplemented, "Not Implemented"},
             {serviceUnavailable, "Service Unavailable"},
             {variantAlsoNegotiates, "Variant Also Negotiates"}}
        };
        for (const auto& [status, name] : names)
            if (status == code)
                return std::string(name);
        return {};
    }
};

/// The request a dynamic route answers: the parsed request, its decoded path and its raw query.
struct RouteRequest {
    const Request&   request;
    std::string_view path;    ///< Percent-decoded, without the query; always starts with the route's prefix.
    std::string_view query;   ///< After the '?', undecoded; empty when there is none.
    std::string_view body{};  ///< The body of a `POST` (see RouteOptions::postBodyLimit); empty for `GET` and `HEAD`.
};

/// What a route handler produces. The server adds `Content-Length` (from `contentLength`) and `Connection`;
/// a handler sets the others (`Content-Type`, `Content-Range`, `Content-Disposition`, `Cache-Control`...).
struct RouteReply {
    Response::StatusCode                             status = Response::ok;
    std::vector<std::pair<std::string, std::string>> headers;
    /// Exact size of the body. Unset with a body: the body ends when the connection closes.
    std::optional<std::uint64_t> contentLength;
    /// Fills the buffer with the next bytes of the body and returns their count; 0 ends the body. Called on
    /// the server's worker thread, one chunk at a time, so memory use does not depend on the body's size.
    /// Never called for `HEAD`. A throw ends the response by closing the connection.
    std::function<std::size_t(std::span<char>)> body;
};

/// Answers `GET` and `HEAD` requests under a prefix, and `POST` when the route accepts it. Runs on the server's
/// worker thread, not on the I/O context: it may block (disk, remote storage) without delaying other connections. A
/// throw answers 500.
using RouteHandler = std::function<RouteReply(const RouteRequest&)>;

/// How a route is served beyond its prefix and handler.
struct RouteOptions {
    /// The path after the prefix carries a secret (a read token): the server never writes it to its logs.
    bool secretPath = false;
    /// Applied to the answers the server generates for this route (4xx, 500), which never pass through the handler.
    std::function<void(RouteReply&)> harden{};
    /// Set, the route also answers `POST` with a body of at most this many bytes, announced by `Content-Length` and
    /// read whole before the handler runs. A request without `Content-Length` is refused with 411, one with a
    /// `Transfer-Encoding` with 501 (no chunked bodies), a malformed length with 400, a longer body with 413; a
    /// connection that ends before the announced length is closed without calling the handler. `Expect: 100-continue`
    /// is answered with `100 Continue` before the body is read.
    std::optional<std::size_t> postBodyLimit{};
};

/// The dynamic routes of a server. Prefixes never overlap, so at most one route matches a path.
class RouteTable {
public:
    struct Entry {
        std::string  prefix;
        RouteHandler handler;
        /// The path after the prefix carries a secret (a read token): the server never writes it to its logs.
        bool secretPath = false;
        /// Applied to the answers the server generates for this route (4xx, 500), which never pass through the handler.
        std::function<void(RouteReply&)> harden;
        /// See RouteOptions::postBodyLimit: unset, the route refuses `POST` with 405.
        std::optional<std::size_t> postBodyLimit;
    };

private:
    struct State {
        std::mutex                          mutex;
        std::vector<std::shared_ptr<Entry>> entries;
    };

public:
    /// Removes its route when destroyed or reset, and stays safe after the table is gone. A request already
    /// running keeps the handler it started with; no later request reaches it.
    class Registration {
    public:
        Registration()                               = default;
        Registration(const Registration&)            = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept : state(std::move(other.state)), entry(std::move(other.entry)) { }
        Registration& operator=(Registration&& other) noexcept {
            if (this != &other) {
                reset();
                state = std::move(other.state);
                entry = std::move(other.entry);
            }
            return *this;
        }
        ~Registration() {
            reset();
        }

        void reset() noexcept {
            if (auto owner = state.lock()) {
                std::lock_guard lock(owner->mutex);
                std::erase(owner->entries, entry);
            }
            state.reset();
            entry.reset();
        }
        [[nodiscard]] explicit operator bool() const noexcept {
            return !state.expired();
        }

    private:
        friend class RouteTable;
        Registration(std::weak_ptr<State> owner, std::shared_ptr<Entry> route) : state(std::move(owner)), entry(std::move(route)) { }
        std::weak_ptr<State>   state;
        std::shared_ptr<Entry> entry;
    };

    /// @param prefix absolute path ending with '/', such as "/webfront/services/files/"; "/" is refused, as is
    ///        anything that would cover the protected "/WebFront.js" (ADR-002).
    /// @throws std::invalid_argument for a malformed prefix, an empty handler, or a prefix that contains
    ///         or is contained in a registered one.
    [[nodiscard]] Registration add(std::string prefix, RouteHandler handler, bool secretPath = false, std::function<void(RouteReply&)> harden = {}) {
        return add(std::move(prefix), std::move(handler), RouteOptions{.secretPath = secretPath, .harden = std::move(harden), .postBodyLimit = {}});
    }

    /// The same, with the options of RouteOptions (`POST` accepted with a bounded body...).
    [[nodiscard]] Registration add(std::string prefix, RouteHandler handler, RouteOptions options) {
        if (!handler)
            throw std::invalid_argument("A route needs a handler");
        if (!validPrefix(prefix))
            throw std::invalid_argument("Invalid route prefix '" + prefix + "'");
        // The log redaction recognizes the services space only: a secret route elsewhere could not be masked reliably.
        if (options.secretPath && !prefix.starts_with("/webfront/services/"))
            throw std::invalid_argument("A secret route must live under /webfront/services/, not '" + prefix + "'");
        auto entry = std::make_shared<Entry>(
            Entry{std::move(prefix), std::move(handler), options.secretPath, std::move(options.harden), options.postBodyLimit});
        std::lock_guard lock(state->mutex);
        for (const auto& other : state->entries)
            if (other->prefix.starts_with(entry->prefix) || entry->prefix.starts_with(other->prefix))
                throw std::invalid_argument("Route prefix '" + entry->prefix + "' collides with '" + other->prefix + "'");
        state->entries.push_back(entry);
        return {state, std::move(entry)};
    }

    /// @return the route whose prefix starts the path, kept alive for as long as the caller holds it.
    [[nodiscard]] std::shared_ptr<const Entry> find(std::string_view path) const {
        std::lock_guard lock(state->mutex);
        for (const auto& entry : state->entries)
            if (path.starts_with(entry->prefix))
                return entry;
        return {};
    }

    /// True for an absolute path ending with '/', other than "/", without "..", "//", "/./", control or reserved characters.
    [[nodiscard]] static bool validPrefix(std::string_view prefix) {
        if (prefix.size() < 2 || !prefix.starts_with('/') || !prefix.ends_with('/'))
            return false;
        if (prefix.find("..") != std::string_view::npos || prefix.find("//") != std::string_view::npos || prefix.find("/./") != std::string_view::npos)
            return false;
        return std::none_of(prefix.begin(), prefix.end(), [](char c) {
            return static_cast<unsigned char>(c) <= 32 || c == 127 || c == '?' || c == '#' || c == '%' || c == '\\';
        });
    }

private:
    std::shared_ptr<State> state = std::make_shared<State>();
};

/// What the resolver of a mount answers for a segment (see Server::mount).
struct MountAnswer {
    /// The routes of the space that the segment names, consulted before the server's own; null when it has none.
    std::shared_ptr<const RouteTable> routes{};
    /// Set, the request is answered with this reply, and nothing of the space is served (unknown space, full server...).
    std::optional<RouteReply> refusal{};
};

/// Called on the server context with the decoded segment that follows the prefix of a mount: `/s/abc/x` gives "abc".
using MountResolver = std::function<MountAnswer(std::string_view segment)>;

/// A reply with a short text body, for a refusal for instance.
[[nodiscard]] inline RouteReply textReply(Response::StatusCode status, std::string text, std::string contentType = "text/plain; charset=utf-8") {
    RouteReply reply;
    reply.status = status;
    reply.headers.emplace_back("Content-Type", std::move(contentType));
    reply.contentLength = text.size();
    reply.body          = [text = std::move(text), sent = std::size_t{0}](std::span<char> out) mutable {
        const auto count = std::min(out.size(), text.size() - sent);
        std::copy_n(text.data() + sent, count, out.data());
        sent += count;
        return count;
    };
    return reply;
}

namespace routing {

/// True when a request target may carry a read token (it names the services space): its text is then never logged,
/// whether or not the target qualifies for route dispatch (a rejected target, an upgrade request...). The target is
/// judged as the server and the file providers would read it: percent-decoded (twice, for a doubly encoded one),
/// with backslashes as separators and empty or "." segments dropped, in any letter case.
inline bool mentionsServices(std::string_view target) {
    constexpr std::string_view marker = "webfront/services";
    std::string                text(target.substr(0, target.find('?')));
    for (int pass = 0; pass < 3; ++pass) {
        std::string lowered;
        lowered.reserve(text.size());
        for (const char c : text)
            lowered += c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string normalized;
        for (std::size_t from = 0; from <= lowered.size();) {
            const auto end     = std::min(lowered.find('/', from), lowered.size());
            const auto segment = std::string_view(lowered).substr(from, end - from);
            if (!segment.empty() && segment != ".")
                normalized += (normalized.empty() ? "" : "/") + std::string(segment);
            from = end + 1;
        }
        if (normalized.find(marker) != std::string::npos)
            return true;
        auto decoded = uri::decode(lowered, false);
        if (decoded == lowered)
            return false;
        text = std::move(decoded);
    }
    return true;  // still encoded after three layers: nothing is worth showing
}

/// What contentLength returns for a length that is not a decimal number, or for several different lengths.
inline constexpr std::uint64_t malformedLength = std::numeric_limits<std::uint64_t>::max();

/// The fields called `name` (case-insensitive), in order, by reference: no list is built, no value is copied.
inline auto fieldsNamed(const Request& request, std::string_view name) {
    return request.headers | std::views::filter([name](const auto& header) {
               return std::ranges::equal(header.name, name, [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
               });
           });
}

/// The text without the optional whitespace (spaces and tabs) around it (RFC 9110 §5.6.3).
inline std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

/// The length announced by `Content-Length`: none without the header, malformedLength for a value that is not a decimal
/// number (or overflows) and for several lengths that disagree (RFC 9112 §6.3). Whitespace around a value is ignored,
/// and a list of equal values, `5, 5` (also what repeated fields amount to), is the one length they all give.
inline std::optional<std::uint64_t> contentLength(const Request& request) {
    std::optional<std::uint64_t> length;
    for (const auto& field : fieldsNamed(request, "Content-Length")) {
        std::string_view rest = field.value;
        for (bool more = true; more;) {
            const auto comma = rest.find(',');
            more             = comma != std::string_view::npos;
            const auto value = trimmed(rest.substr(0, comma));
            if (more)
                rest.remove_prefix(comma + 1);
            std::uint64_t parsed    = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || error != std::errc{} || end != value.data() + value.size() || parsed == malformedLength)
                return malformedLength;
            if (length && *length != parsed)
                return malformedLength;
            length = parsed;
        }
    }
    return length;
}

/// Whether the field `name` is present, whatever its value.
inline bool hasField(const Request& request, std::string_view name) {
    return !std::ranges::empty(fieldsNamed(request, name));
}

/// Whether the client waits for `100 Continue` before it sends the body: `Expect` lists `100-continue` (RFC 9110 §10.1.1:
/// a comma-separated list, each element a token compared without case, not a substring), from an HTTP/1.1 or later
/// client, as an interim answer is never sent to 1.0.
inline bool expectsContinue(const Request& request) {
    if (request.httpVersionMajor < 1 || (request.httpVersionMajor == 1 && request.httpVersionMinor < 1))
        return false;
    constexpr std::string_view token = "100-continue";
    for (const auto& field : fieldsNamed(request, "Expect")) {
        std::string_view rest = field.value;
        for (bool more = true; more;) {
            const auto comma = rest.find(',');
            more             = comma != std::string_view::npos;
            const auto text  = trimmed(rest.substr(0, comma));
            if (more)
                rest.remove_prefix(comma + 1);
            if (std::ranges::equal(text, token, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == b;
                }))
                return true;
        }
    }
    return false;
}

/// The path without empty and "." segments, so that "/a//b/./c" reaches the route of "/a/b/c" instead of falling
/// through to the file providers, which would resolve it to the same file. A trailing slash is kept.
inline std::string collapseSegments(std::string path) {
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '\\', '/');  // the providers resolve a backslash as a separator there
#endif
    std::string collapsed;
    for (std::size_t start = 0; start < path.size();) {
        const auto end     = path.find('/', start);
        const auto segment = std::string_view(path).substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!segment.empty() && segment != ".") {
            collapsed += '/';
            collapsed += segment;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    if (collapsed.empty() || path.ends_with('/'))
        collapsed += '/';
    return collapsed;
}

/// The normalized path and raw query of an origin-form request target, or nothing when it is not a safe one.
/// The path is percent-decoded and a literal '+' stays a '+', as in any URL path.
inline std::optional<std::pair<std::string, std::string>> splitTarget(const std::string& uri) {
    const auto mark    = uri.find('?');
    const auto decoded = uri::decode(std::string_view(uri).substr(0, mark), false);
    if (!decoded.starts_with('/') || decoded.find("..") != std::string::npos)
        return {};
    if (std::any_of(decoded.begin(), decoded.end(), [](char c) {
            return static_cast<unsigned char>(c) < 32 || c == 127;
        }))
        return {};
    return std::pair{collapseSegments(decoded), mark == std::string::npos ? std::string{} : uri.substr(mark + 1)};
}

/// The segments of a path as it came, each percent-decoded on its own, without the empty and "." ones (as collapseSegments
/// drops them): "/s/a%2Fb/./c" gives "s", "a/b" and "c", where decoding the whole path first would give four segments.
inline std::vector<std::string> decodedSegments(std::string_view raw) {
    std::vector<std::string> segments;
    for (std::size_t start = 0; start <= raw.size();) {
#ifdef _WIN32
        const auto end = std::min(raw.find_first_of("/\\", start), raw.size());  // a separator for the providers there
#else
        const auto end = std::min(raw.find('/', start), raw.size());
#endif
        auto decoded = uri::decode(raw.substr(start, end - start), false);
        if (!decoded.empty() && decoded != ".")
            segments.push_back(std::move(decoded));
        start = end + 1;
    }
    return segments;
}

/// Refuses what could split a response or contradict the framing the server owns.
inline void validateReply(const RouteReply& reply) {
    const auto isToken = [](std::string_view name) {
        return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            // RFC 9110 tchar, spelled out: std::isalnum depends on the locale and may accept non-ASCII bytes.
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                   || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
        });
    };
    for (const auto& [name, value] : reply.headers) {
        if (!isToken(name))
            throw std::runtime_error("Invalid response header name '" + name + "'");
        if (std::any_of(value.begin(), value.end(), [](char c) {
                return c == '\r' || c == '\n' || c == '\0';
            }))
            throw std::runtime_error("Invalid value for response header '" + name + "'");
        for (const auto reserved : {std::string_view("Content-Length"), std::string_view("Connection"), std::string_view("Transfer-Encoding")})
            if (std::ranges::equal(name, reserved, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                }))
                throw std::runtime_error("Response header '" + name + "' belongs to the server");
    }
}

}  // namespace routing

/// The reply of a route that serves `file` as `contentType`, with the byte ranges of RFC 9110 §14: `Range` selects
/// one part (206 with `Content-Range`, 416 when it starts past the end), `If-Range` conditions it on the file's
/// entity tag, several ranges are answered in full, and only the selected bytes are read, in chunks, by the server.
/// A file that is encoded or has no known size is answered in full with `Accept-Ranges: none`.
/// The handler adds what it alone knows (`Cache-Control`, `Content-Disposition`...).
[[nodiscard]] inline RouteReply fileReply(const Request& request, fs::File file, std::string contentType) {
    RouteReply reply;
    reply.headers.emplace_back("Content-Type", std::move(contentType));
    const auto size = file.knownSize();
    if (file.isEncoded()) {
        // The stored bytes are sent as they are: the client must accept their coding (no decoding here).
        reply.headers.emplace_back("Vary", "Accept-Encoding");
        if (!request.acceptsEncoding(file.getEncoding())) {
            reply.status        = Response::notAcceptable;
            reply.contentLength = 0;
            return reply;
        }
        reply.headers.emplace_back("Content-Encoding", std::string(file.getEncoding()));
    }
    if (file.isEncoded() || !size) {
        reply.headers.emplace_back("Accept-Ranges", "none");
        reply.body = [file = std::make_shared<fs::File>(std::move(file))](std::span<char> out) {
            return file->read(out);
        };
        reply.contentLength = size;
        return reply;
    }
    reply.headers.emplace_back("Accept-Ranges", "bytes");
    if (file.entityTag())
        reply.headers.emplace_back("ETag", *file.entityTag());
    // Range is defined for GET only (RFC 9110 §14.2): HEAD describes the full response.
    const auto selection = request.method == Request::Method::Get ? range::select(request, *size, file.entityTag()) : range::Selection{};
    if (selection.outcome == range::Outcome::unsatisfiable) {
        reply.status = Response::rangeNotSatisfiable;
        reply.headers.emplace_back("Content-Range", range::unsatisfiedContentRange(*size));
        reply.contentLength = 0;
        return reply;
    }
    std::uint64_t first  = 0;
    std::uint64_t length = *size;
    if (selection.outcome == range::Outcome::partial) {
        reply.status = Response::partialContent;
        reply.headers.emplace_back("Content-Range", range::contentRange(selection, *size));
        first  = selection.first;
        length = selection.length;
    }
    reply.contentLength = length;
    reply.body = [file = std::make_shared<fs::File>(std::move(file)), first, remaining = length, positioned = first == 0](std::span<char> out) mutable {
        if (!positioned) {
            if (!file->seek(first))
                throw std::runtime_error("The file cannot seek");
            positioned = true;
        }
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), remaining));
        if (count == 0)
            return std::size_t{0};
        const auto read = file->read(out.first(count));
        remaining      -= read;
        return read;
    };
    return reply;
}

template <networking::Features Net, fs::Provider FS, BuffersPolicyType Policy = DefaultBuffersPolicy>
class RequestHandler {
public:
    explicit RequestHandler(std::filesystem::path root, detail::BoundContentPolicy policy = detail::BoundContentPolicy{})
        : fs(root), contentPolicy(std::move(policy)) { }
    ~RequestHandler()                                = default;
    RequestHandler(const RequestHandler&)            = default;
    RequestHandler(RequestHandler&&)                 = default;
    RequestHandler& operator=(const RequestHandler&) = default;
    RequestHandler& operator=(RequestHandler&&)      = default;

    /** @brief Per-handler gzip cache counters and retained storage; read on the server context. */
    [[nodiscard]] gzip::ValidatedCache::Statistics fallbackCacheStatistics() const {
        return fallbackCache.statistics();
    }

    struct Prepared {
        Response response;
        bool     needsDecode      = false;
        bool     head             = false;
        bool     identityFallback = false;
        /// An unencoded file of known size, left unread to be streamed (asynchronous requests only).
        std::unique_ptr<fs::File> file{};
        /// The part of `file` a `Range` request selected: its first byte, and how many bytes follow (206).
        bool          partial = false;
        std::uint64_t offset  = 0;
        std::uint64_t length  = 0;
        std::size_t   limit   = maxDecodedAssetSize<Policy>();
    };

    // Explicit synchronous low-level API; Server always uses prepareRequest and the owned worker.
    Response handleRequest(Request request) {
        return prepare(std::move(request), false).response;
    }
    Prepared prepareRequest(Request request) {
        return prepare(std::move(request), true);
    }

    template <typename Lease>
    Response finish(Prepared prepared, Lease& lease) {
        if (!lease)
            return encodingError(Response::serviceUnavailable);
        if (!lease.valid() || lease.decoded().size() > maxDecodedAssetSize<Policy>())
            return encodingError(Response::internalServerError);
        fallbackCache.remember(lease.encoded(), lease.decoded());
        prepared.response.content = lease.takeBody();
        prepared.response.headers.emplace_back("Content-Length", std::to_string(lease.decoded().size()));
        return std::move(prepared.response);
    }

private:
    Prepared prepare(Request request, bool asynchronous) {
        auto requestPath = filePath(request.uri);
        if (!requestPath)
            return {Response::getStatusResponse(Response::badRequest)};
        switch (request.method) {
            case Request::Method::Get:
                if (auto upgrade = webSocketUpgrade(request))
                    return {std::move(*upgrade)};
                [[fallthrough]];
            case Request::Method::Head:
                break;
            default:
                return {Response::getStatusResponse(Response::notImplemented)};
        }
        Prepared result;
        result.head    = request.method == Request::Method::Head;
        auto& response = result.response;
        if (auto error = readFile(request, *requestPath, result, asynchronous)) {
            response           = std::move(*error);
            result.needsDecode = false;
            return result;
        }
        response.statusCode = result.partial ? Response::partialContent : Response::ok;
        response.headers.emplace_back("Content-Type", MimeType(requestPath->extension().string()).toString());
        if (!result.needsDecode) {
            if (!response.getHeaderValue("Content-Length"))
                response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
            if (result.head)
                response.content.clear();
        }
        // Only an unencoded file of known size is served by ranges, and it announced them when it was opened.
        if (!response.getHeaderValue("Accept-Ranges"))
            response.headers.emplace_back("Accept-Ranges", "none");
        return result;
    }

    // The file a request URI names, or nothing when the URI is not a safe absolute path.
    static std::optional<std::filesystem::path> filePath(const std::string& uri) {
        // The query is not part of the file name; it is cut before decoding, so "%3F" stays in the name.
        const auto rawPath    = std::string_view(uri).substr(0, uri.find('?'));
        auto       requestUri = uri::decode(rawPath);
        const bool secret     = routing::mentionsServices(uri) || routing::mentionsServices(requestUri);
        if (!secret)
            log::debug("Request path - raw:'{}' decoded:'{}'", rawPath, requestUri);  // never the query
        if (!isSafePath(requestUri))
            return {};
        auto requestPath = std::filesystem::path(requestUri).relative_path();
        if (!secret)
            log::debug("  -> requestPath : {}", requestPath.string());
        if (!requestPath.has_filename())
            requestPath /= "index.html";
        return requestPath;
    }

    static bool isSafePath(std::string_view path) {
        if (!path.starts_with('/'))
            return false;
        return path.find("..") == std::string_view::npos;
    }

    std::optional<Response> webSocketUpgrade(const Request& request) const {
        if (!request.isUpgradeRequest("websocket"))
            return {};
        const auto origins = request.getHeadersValues("Origin");
        if (origins.size() > 1 || !contentPolicy.allowsBridge(origins.empty() ? std::nullopt : std::optional<std::string_view>{origins.front()}))
            return rejectWebSocketOrigin();
        auto key = request.getHeaderValue("Sec-WebSocket-Key");
        if (!key)
            return {};
        Response response;
        response.statusCode = Response::StatusCode::switchingProtocols;
        response.headers.emplace_back("Upgrade", "websocket");
        response.headers.emplace_back("Connection", "Upgrade");
        response.headers.emplace_back("Sec-WebSocket-Accept", websocket::getHashedSecKey(key.value()));
        response.headers.emplace_back("Sec-WebSocket-Protocol", "WebFront_0.1");
        return response;
    }

    static Response rejectWebSocketOrigin() {
        constexpr std::string_view reason = "WebFront: WebSocket Origin rejected by content policy. Check contentPolicy.bridgeAllowedOrigins "
                                            "and allowMissingOrigin; wildcard binds authorize loopback only.";
        log::warn("{}", reason);
        Response response;
        response.statusCode = Response::forbidden;
        response.content    = std::string(reason) + "\n";
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
        return response;
    }

    // Reads the file into the response content, or returns the error response.
    std::optional<Response> readFile(const Request& request, const std::filesystem::path& requestPath, Prepared& prepared, bool asynchronous) {
        auto file = fs.open(requestPath);
        if (!file)
            return Response::getStatusResponse(Response::notFound);
        auto&      response       = prepared.response;
        const bool fallback       = file->isEncoded() && encoding::isGzip(file->getEncoding()) && !request.acceptsEncoding(file->getEncoding())
                                    && request.acceptsEncoding("identity");
        prepared.identityFallback = fallback;
        if (asynchronous && !file->isEncoded()) {
            if (const auto size = file->knownSize()) {
                response.headers.emplace_back("Accept-Ranges", "bytes");
                if (file->entityTag())
                    response.headers.emplace_back("ETag", *file->entityTag());
                // Range is defined for GET only (RFC 9110 §14.2): HEAD describes the full response.
                const auto selection = request.method == Request::Method::Get ? range::select(request, *size, file->entityTag()) : range::Selection{};
                if (selection.outcome == range::Outcome::unsatisfiable) {
                    // As in fileReply: the status and Content-Range say it all, there is no payload.
                    Response unsatisfiable;
                    unsatisfiable.statusCode = Response::rangeNotSatisfiable;
                    unsatisfiable.headers.emplace_back("Content-Length", "0");
                    unsatisfiable.headers.emplace_back("Content-Range", range::unsatisfiedContentRange(*size));
                    unsatisfiable.headers.emplace_back("Accept-Ranges", "bytes");
                    return unsatisfiable;
                }
                prepared.length = *size;
                if (selection.outcome == range::Outcome::partial) {
                    prepared.partial = true;
                    prepared.offset  = selection.first;
                    prepared.length  = selection.length;
                    response.headers.emplace_back("Content-Range", range::contentRange(selection, *size));
                }
                response.headers.emplace_back("Content-Length", std::to_string(prepared.length));
                if (prepared.head)
                    return {};
                prepared.file = std::make_unique<fs::File>(std::move(*file));
                return {};
            }
        }
        const auto            inputLimit = asynchronous && fallback ? maxEncodedAssetSize : std::numeric_limits<std::size_t>::max();
        std::array<char, 512> buffer{};
        while (auto bytesRead = file->read(buffer)) {
            if (bytesRead > inputLimit - response.content.size())
                return encodingError(Response::internalServerError);
            response.content.append(buffer.data(), bytesRead);
        }
        if (!file->isEncoded())
            return {};
        return negotiateEncoding(request, file->getEncoding(), prepared, asynchronous);
    }

    static Response encodingError(Response::StatusCode code) {
        auto error = Response::getStatusResponse(code);
        error.headers.emplace_back("Vary", "Accept-Encoding");
        return error;
    }

    std::optional<Response> negotiateEncoding(const Request& request, std::string_view coding, Prepared& prepared, bool asynchronous) {
        auto& response = prepared.response;
        response.headers.emplace_back("Vary", "Accept-Encoding");
        if (request.acceptsEncoding(coding)) {
            response.headers.emplace_back("Content-Encoding", coding);
            return {};
        }
        if (!request.acceptsEncoding("identity") || !encoding::isGzip(coding))
            return encodingError(Response::notAcceptable);
        if (asynchronous) {
            if (auto cached = fallbackCache.lookup(response.content, prepared.limit, prepared.head)) {
                if (!cached->allowed)
                    return encodingError(Response::internalServerError);
                response.headers.emplace_back("Content-Length", std::to_string(cached->length));
            } else {
                prepared.needsDecode = true;
            }
            return {};
        }
        auto decoded = fallbackCache.resolve(response.content, maxDecodedAssetSize<Policy>(), request.method == Request::Method::Head);
        if (!decoded)
            return encodingError(Response::internalServerError);
        response.headers.emplace_back("Content-Length", std::to_string(*decoded));
        return {};
    }

    FS                         fs;
    detail::BoundContentPolicy contentPolicy;
    gzip::ValidatedCache       fallbackCache{maxDecodedAssetCacheSize<Policy>()};
    std::size_t                maxEncodedAssetSize{gzip::decodeLimits<Policy>().encoded};
};

template <networking::Features Net, fs::Provider FS, BuffersPolicyType Policy = DefaultBuffersPolicy, typename Decode = gzip::Decoder>
class Server {
public:
    Server(std::string_view                  address,
           std::string_view                  port,
           std::filesystem::path             docRoot     = ".",
           ContentPolicy                     policy      = {},
           Decode                            decode      = {},
           std::shared_ptr<tls::Credentials> credentials = {})
        : acceptor(ioContext),
          listeningPort((configureTransport(std::move(credentials)), bind(address, port))),
          requestHandler(docRoot,
                         detail::BoundContentPolicy{detail::applicationOrigin(address, listeningPort, networking::secureTransport<Net>), std::move(policy)}),
          decodeExecutor(gzip::decodeLimits<Policy>(), std::move(decode)) {
        spawn(acceptConnections());
    }
    ~Server() noexcept {
        drainUntilJoined();
    }
    Server(const Server&)            = delete;
    Server(Server&&)                 = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&)      = delete;

    /// The bound port, including the OS-assigned port when constructed with "0".
    const std::string& port() const noexcept {
        return listeningPort;
    }

    void run() {
        ioContext.run();
    }
    void runOne() {
        ioContext.run_one();
    }

    /// Thread-safe. Cancels the acceptor and the connections, and makes run() return.
    void stop() {
        log::info("Stopping HTTP server...");
        tasks.requestStop();
        decodeExecutor.stop();
        routeWorker.stop();
        fileWorker.stop();
        ioContext.stop();
    }

    /// Runs the cancelled tasks to completion, so that no connection outlives the objects its handlers use.
    /// To be called from the thread owning the context, once nothing else runs it.
    void drain() {
        decodeExecutor.stop();
        decodeExecutor.join();
        // The cancelled tasks are pumped before the workers are joined: a handler may wait for what the cleanup
        // of a cancelled task does, and the tasks awaiting a running job end when that job returns.
        routeWorker.stop();
        fileWorker.stop();
        // A task awaiting a job the workers are still running has no work in the context until that job returns: it is
        // pending, not stalled. Only a scope that stays idle while the workers did nothing is reported (std::logic_error).
        for (;;) {
            const auto route = routeWorker.activity(), file = fileWorker.activity();
            try {
                tasks.drain(ioContext);
                break;
            } catch (const std::logic_error&) {
                if (!route.busy && !file.busy && route == routeWorker.activity() && file == fileWorker.activity())
                    throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        routeWorker.join();
        fileWorker.join();
    }

    /** @brief Destructor-safe join, retaining ownership until every task has finished. */
    void drainUntilJoined() noexcept {
        decodeExecutor.stop();
        decodeExecutor.join();
        routeWorker.stop();
        fileWorker.stop();
        tasks.drainUntilJoined(ioContext);
        routeWorker.join();
        fileWorker.join();
    }

    /** @brief Scope shared with upgraded connections; the server must outlive them. */
    networking::TaskScope& taskScope() noexcept {
        return tasks;
    }

    /** @brief Starts an application coroutine on the server context and owns it through shutdown. */
    TaskHandle startTask(Task task) {
        TaskHandle handle;
        // Register cancellation before associating, including a concurrent close.
        auto registration = std::make_unique<detail::TaskRegistration>(handle, tasks.stopToken());
        if (!networking::Spawn(tasks, Net::Scheduler(acceptor), detail::ownedTask(std::move(task), std::move(registration))))
            handle.requestStop();
        return handle;
    }

    /** @brief A sender completing on the server context after the duration, or stopped first by its stop token. */
    template <typename Rep, typename Period>
    auto resumeAfter(std::chrono::duration<Rep, Period> duration) {
        return Net::ResumeAfter(acceptor, duration);
    }

    /// Answers the `GET` and `HEAD` requests under `prefix` with `handler` (see RouteTable::add for the
    /// prefix rules). The route lasts as long as the returned registration; it never masks the embedded
    /// `WebFront.js`, and takes precedence over the file providers. Thread-safe.
    [[nodiscard]] RouteTable::Registration
    addRoute(std::string prefix, RouteHandler handler, bool secretPath = false, std::function<void(RouteReply&)> harden = {}) {
        return routes.add(std::move(prefix), std::move(handler), secretPath, std::move(harden));
    }

    /// The same, with the options of RouteOptions (`POST` accepted with a bounded body...).
    [[nodiscard]] RouteTable::Registration addRoute(std::string prefix, RouteHandler handler, RouteOptions options) {
        return routes.add(std::move(prefix), std::move(handler), std::move(options));
    }

    /// Called, on the context's thread, with the socket of a connection upgraded to a WebSocket. A handler that also takes a
    /// std::string_view receives the space of the upgrade: the path of its mount and segment ("/s/abc/"), empty outside.
    template <typename Handler>
    void onUpgrade(Handler&& handler) {
        if constexpr (std::invocable<Handler&, typename Net::Socket&&, std::string_view>)
            upgradeHandler = std::forward<Handler>(handler);
        else
            upgradeHandler = [forward = std::forward<Handler>(handler)](typename Net::Socket&& socket, std::string_view) mutable {
                forward(std::move(socket));
            };
    }

    /** @brief Serves the paths under `prefix` (absolute, ending with '/', as a route prefix) as a family of spaces: a request
     * for `prefix + segment + "/" + rest`, a WebSocket upgrade included, belongs to the space `segment`. `resolver` is
     * called on the server context with the decoded segment of each such request; unless it refuses, the request is
     * served as `"/" + rest` would be at the root (the space's routes, then the server's, then the file providers), and an
     * upgrade hands its socket to the onUpgrade handler with the space's path. `prefix + segment`, without the slash that
     * relative URLs need, is redirected to it. Prefixes of two mounts never overlap (std::invalid_argument). Thread-safe.
     */
    void mount(std::string prefix, MountResolver resolver) {
        if (!resolver)
            throw std::invalid_argument("A mount needs a resolver");
        if (!RouteTable::validPrefix(prefix))
            throw std::invalid_argument("Invalid mount prefix '" + prefix + "'");
        std::scoped_lock lock{mountsMutex};
        for (const auto& other : mounts)
            if (other.first.starts_with(prefix) || prefix.starts_with(other.first))
                throw std::invalid_argument("Mount prefix '" + prefix + "' collides with '" + other.first + "'");
        mounts.emplace_back(std::move(prefix), std::make_shared<const MountResolver>(std::move(resolver)));
    }

    [[nodiscard]] auto fallbackDecodeStatistics() const {
        return decodeExecutor.statistics();
    }
    [[nodiscard]] auto fallbackCacheStatistics() const {
        auto result    = requestHandler.fallbackCacheStatistics();
        result.decodes = decodeExecutor.statistics().decodes;
        return result;
    }

private:
    typename Net::IoContext         ioContext;
    typename Net::Acceptor          acceptor;
    std::string                     listeningPort;
    networking::TaskScope           tasks;
    RequestHandler<Net, FS, Policy> requestHandler;
    gzip::DecodeExecutor<Decode>    decodeExecutor;
    RouteTable                      routes;
    // Route handlers may block for as long as they like: they get their own threads, so that the files of the
    // providers (WebFront.js, pages) keep flowing for the other browsers.
    BlockingWorker                                                            routeWorker{4};
    BlockingWorker                                                            fileWorker{2};
    std::function<void(typename Net::Socket&&, std::string_view)>             upgradeHandler;
    mutable std::mutex                                                        mountsMutex;  // guards mounts, added from any thread
    std::vector<std::pair<std::string, std::shared_ptr<const MountResolver>>> mounts;

    /// A request under a mount: the reply that answers it instead (refusal, redirection), or what it is served as.
    struct Mounted {
        std::optional<RouteReply>         reply;
        std::string                       space;  ///< The path of the mount and segment, "/s/abc/".
        std::string                       uri;    ///< The target within the space, encoded again: "/rest?query".
        std::shared_ptr<const RouteTable> routes;
    };

    std::optional<Mounted> resolveMount(const Request& request) const {
        std::string                                        prefix;
        std::shared_ptr<const MountResolver>               resolver;
        std::optional<std::pair<std::string, std::string>> split;
        {
            std::scoped_lock lock{mountsMutex};
            if (mounts.empty())
                return {};
            split = routing::splitTarget(request.uri);
            if (!split)
                return {};
            for (const auto& [candidate, found] : mounts)
                if (split->first.starts_with(candidate) && split->first.size() > candidate.size()) {
                    prefix   = candidate;
                    resolver = found;
                }
        }
        if (!resolver)
            return {};
        const auto& query = split->second;
        Mounted     mounted;
        const auto  notFound = [&mounted] {
            mounted.reply = textReply(Response::notFound, "Not Found\n");
            return mounted;
        };
        // The segment is cut in the path as it came, each segment decoded on its own: an encoded separator ("%2F") stays
        // inside its segment, where it names no space, instead of becoming a boundary that would select another one.
        const auto raw            = std::string_view(request.uri).substr(0, request.uri.find('?'));
        const auto segments       = routing::decodedSegments(raw);
        const auto prefixSegments = routing::decodedSegments(prefix);
        const auto depth          = prefixSegments.size();
        if (segments.size() <= depth || !std::equal(prefixSegments.begin(), prefixSegments.end(), segments.begin()))
            return notFound();  // under the prefix once decoded, but not segment by segment
        const auto& segment = segments[depth];
        if (segment.find_first_of("/\\") != std::string::npos || segment == "..")
            return notFound();
        const bool trailingSlash = raw.ends_with('/');
        if (segments.size() == depth + 1 && !trailingSlash) {
            // The relative URLs of a page resolve against its directory: "/s/abc" must become "/s/abc/".
            if (request.isUpgradeRequest("websocket"))
                return notFound();
            mounted.reply = textReply(Response::movedPermanently, "");
            mounted.reply->headers.emplace_back("Location", prefix + uri::encode(segment) + "/" + (query.empty() ? "" : "?" + query));
            return mounted;
        }
        std::string rest = "/";
        for (auto index = depth + 1; index < segments.size(); ++index)
            rest += uri::encode(segments[index]) + (index + 1 < segments.size() || trailingSlash ? "/" : "");
        MountAnswer answer;
        try {
            answer = (*resolver)(segment);
        } catch (const std::exception& error) {
            log::warn("The resolver of the mount {} failed: {}", prefix, error.what());
            answer.refusal = textReply(Response::internalServerError, "Internal Server Error\n");
        } catch (...) {
            log::warn("The resolver of the mount {} failed with an exception which is not a std::exception", prefix);
            answer.refusal = textReply(Response::internalServerError, "Internal Server Error\n");
        }
        if (answer.refusal) {
            mounted.reply = std::move(answer.refusal);
            return mounted;
        }
        mounted.space  = prefix + segment + "/";
        mounted.uri    = rest + (query.empty() ? "" : "?" + query);
        mounted.routes = std::move(answer.routes);
        return mounted;
    }

    // Sends a reply the server did not get from a route (a mount's refusal or redirection), then closes the connection.
    networking::Task sendReply(typename Net::Socket& socket, bool head, RouteReply reply) {
        Response response;
        try {
            routing::validateReply(reply);
            response.statusCode = reply.status;
            for (auto& [name, value] : reply.headers)
                response.headers.emplace_back(name, value);
            if (reply.contentLength)
                response.headers.emplace_back("Content-Length", std::to_string(*reply.contentLength));
            else if (!reply.body)
                response.headers.emplace_back("Content-Length", "0");
        } catch (const std::exception& error) {
            log::warn("A mount's reply is invalid: {}", error.what());
            response   = Response::getStatusResponse(Response::internalServerError);
            reply.body = nullptr;
        }
        if (head)
            response.content.clear();
        co_await Net::Send(socket, response.toBuffers<Net>());
        if (!head && reply.body)
            co_await streamBody(routeWorker, socket, std::move(reply.body), reply.contentLength, "Mount reply");
        co_await closeResponse(socket);
    }

    void configureTransport(std::shared_ptr<tls::Credentials> credentials) {
        if constexpr (networking::secureTransport<Net>) {
            if (!credentials)
                throw std::invalid_argument("WebFront: missing TLS credentials");
            acceptor.credentials = std::move(credentials);
        } else if (credentials) {
            throw std::invalid_argument("WebFront: TLS credentials require the HTTPS provider");
        }
    }

    // Cancels a handshake that outlives Net::handshakeTimeout, unless `settled` ends the wait first.
    networking::Task handshakeDeadline(std::shared_ptr<networking::ex::inplace_stop_source> expired,
                                       std::shared_ptr<networking::ex::inplace_stop_source> settled) {
        if constexpr (networking::secureTransport<Net>)
            co_await networking::ex::write_env(Net::ResumeAfter(acceptor, Net::handshakeTimeout),
                                               networking::ex::prop{networking::ex::get_stop_token, settled->get_token()});
        expired->request_stop();
    }

    networking::Task closeResponse(typename Net::Socket& socket) {
        if constexpr (networking::secureTransport<Net>)
            co_await Net::Close(socket);
        else
            socket.shutdown(Net::Socket::shutdown_both);
        co_return;
    }

    std::string bind(std::string_view address, std::string_view requestedPort) {
        try {
            const auto endpoint = Net::Resolve(address, requestedPort);
            acceptor.open(endpoint.protocol());
#ifdef _WIN32
            // SO_REUSEADDR lets Winsock bind a second socket to a port in use and then deals the connections out unpredictably, so
            // two applications would answer each other's requests. SO_EXCLUSIVEADDRUSE makes the second bind fail instead.
            if constexpr (requires { acceptor.native_handle(); }) {
                const BOOL exclusive = TRUE;
                if (::setsockopt(static_cast<SOCKET>(acceptor.native_handle()),
                                 SOL_SOCKET,
                                 SO_EXCLUSIVEADDRUSE,
                                 reinterpret_cast<const char*>(&exclusive),
                                 sizeof exclusive)
                    != 0)
                    throw std::system_error(::WSAGetLastError(), std::system_category(), "SO_EXCLUSIVEADDRUSE");
            }
#else
            acceptor.set_option(typename Net::Acceptor::reuse_address(true));  // rebinding after a restart; POSIX still refuses a port in use
#endif
            acceptor.bind(endpoint);
            acceptor.listen();
        } catch (const std::system_error& error) {
            // Name the address and port: the bare "bind: Address already in use" does not say which server failed.
            throw std::system_error(error.code(),
                                    "WebFront cannot listen on " + std::string(address) + ":" + std::string(requestedPort) + ": " + error.code().message()
                                        + (error.code() == std::errc::address_in_use
                                               ? " (another instance may use this port; set WEBFRONT_PORT=0 or WebFrontConfig::port to \"0\" for a free port)"
                                               : ""));
        }
        // The port really bound, whatever the request said: the resolver also accepts "00", "65536" or a service name.
        if constexpr (requires { acceptor.local_endpoint().port(); })
            return std::to_string(acceptor.local_endpoint().port());
        else
            return std::string(requestedPort);  // Preserve legacy providers/mocks which have no endpoint query.
    }

    void spawn(networking::Task task) {
        networking::Spawn(tasks, Net::Scheduler(acceptor), std::move(task));
    }

    // A failed accept (an aborted connection, no file descriptor left...) must not end the accepting.
    networking::Task acceptConnections() {
        while (!tasks.stopRequested()) {
            try {
                spawn(serve(co_await Net::Accept(acceptor)));
            } catch (const std::system_error& error) {
                if (error.code() == std::errc::operation_canceled)
                    co_return;
                log::error("Accept failed: {}", error.what());
            }
        }
    }

    struct RouteMatch {
        std::shared_ptr<const RouteTable::Entry> route;
        std::string                              path, query;
    };

    // A request, other than a WebSocket upgrade, under a registered prefix. Every method under a prefix
    // belongs to the route (405 for those it does not serve): its URLs never fall through to the providers.
    // The routes of a space (`own`) come before the server's.
    std::optional<RouteMatch> matchRoute(const Request& request, const RouteTable* own = nullptr) const {
        if (request.isUpgradeRequest("websocket"))
            return {};
        auto split = routing::splitTarget(request.uri);
        if (!split)
            return {};
        auto route = own ? own->find(split->first) : nullptr;
        if (!route)
            route = routes.find(split->first);
        if (!route)
            return {};
        return RouteMatch{std::move(route), std::move(split->first), std::move(split->second)};
    }

    // Sends a body read on the worker in chunks of at most emissionBufferSize bytes, each read after the
    // previous one was sent (back-pressure). A failure cannot change the status line already sent: it is
    // logged, and the caller closes the connection, which the client sees as a truncated body.
    networking::Task streamBody(BlockingWorker&                             worker,
                                typename Net::Socket&                       socket,
                                std::function<std::size_t(std::span<char>)> body,
                                std::optional<std::uint64_t>                length,
                                std::string                                 label,
                                bool                                        secret = false) {
        try {
            std::vector<char> chunk(Policy::emissionBufferSize);
            std::uint64_t     sent = 0;
            for (;;) {
                std::size_t count = 0;
                co_await networking::ex::continues_on(worker.run([&] {
                    count = body(chunk);
                }),
                                                      Net::Scheduler(socket));
                if (count == 0)
                    break;
                if (count > chunk.size() || (length && count > *length - sent))
                    throw std::length_error("The body is longer than announced");
                sent += count;
                std::vector<typename Net::ConstBuffer> buffers{Net::Buffer(static_cast<const char*>(chunk.data()), count)};
                co_await Net::Send(socket, buffers);
            }
            if (length && sent != *length)
                throw std::length_error("The body is shorter than announced");
        } catch (const std::exception& error) {
            // The message of a handler can quote the request path, which holds a token on a secret route.
            if (secret)
                log::warn("{} ended its response early (the error is not logged)", label);
            else
                log::warn("{} ended its response early: {}", label, error.what());
        } catch (...) {
            log::warn("{} ended its response early: unknown exception", label);
        }
    }

    // Runs the handler on the worker, then streams its body in chunks of at most emissionBufferSize bytes,
    // each read on the worker and sent before the next is read (back-pressure). Closing the connection
    // fails the pending send and ends the response; the stop request of the server cancels the awaits.
    // `received` views the bytes that followed the headers in `buffer`, the reception buffer of serve(), which outlives this
    // call: nothing is copied unless a POST needs the rest of its body, and then only the bytes the body owns.
    networking::Task serveRoute(typename Net::Socket& socket, Request request, RouteMatch match, std::string_view received, std::span<char> buffer) {
        const bool head = request.method == Request::Method::Head;
        const bool post = request.method == Request::Method::Post && match.route->postBodyLimit.has_value();
        RouteReply reply;
        Response   response;
        // The route's policy for the answers the server writes itself; the type and disposition stay those of the error page.
        const auto sameHeader = [](std::string_view a, std::string_view b) {
            return std::ranges::equal(a, b, [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        };
        const auto hardenError = [&] {
            if (!match.route->harden)
                return;
            RouteReply policy;
            match.route->harden(policy);
            for (auto& [name, value] : policy.headers)
                if (!sameHeader(name, "Content-Type") && !sameHeader(name, "Content-Disposition"))
                    response.headers.emplace_back(name, value);
        };
        // The body of a POST outlives the handler: its answer may stream from the view it was given. `body` holds it only
        // when it did not come with the headers (then `view` is in the reception buffer of serve()); both live as long as this call.
        std::string      body;
        std::string_view view;
        try {
            std::optional<Response::StatusCode> refusal;
            if (post) {
                const auto length = routing::contentLength(request);
                if (routing::hasField(request, "Transfer-Encoding"))
                    refusal = Response::notImplemented;  // no chunked bodies
                else if (!length)
                    refusal = Response::lengthRequired;
                else if (*length == routing::malformedLength)
                    refusal = Response::badRequest;
                else if (*length > *match.route->postBodyLimit)
                    refusal = Response::payloadTooLarge;
                else {
                    if (received.size() < *length && routing::expectsContinue(request)) {
                        // The client waits for this interim answer before it sends the body (curl does beyond 1 KiB).
                        const std::array<typename Net::ConstBuffer, 1> proceed{Net::Buffer(std::string_view{"HTTP/1.1 100 Continue\r\n\r\n"})};
                        co_await Net::Send(socket, proceed);
                    }
                    if (received.size() >= *length) {
                        view = received.substr(0, *length);  // the body came with the headers; what follows would be another request
                    } else {
                        // A single allocation for a small body; a large one grows with what the client really sends.
                        body.reserve(std::min<std::size_t>(*length, buffer.size()));
                        body.assign(received);
                        while (body.size() < *length) {
                            const auto wanted = std::min<std::size_t>(*length - body.size(), buffer.size());
                            const auto count  = co_await Net::Receive(socket, Net::Buffer(buffer.data(), wanted));
                            if (count == 0) {  // the client ended the connection before the announced length
                                co_await closeResponse(socket);
                                co_return;
                            }
                            body.append(buffer.data(), count);
                        }
                        view = body;
                    }
                }
            }
            if (!head && request.method != Request::Method::Get && !post) {
                response = Response::getStatusResponse(Response::methodNotAllowed);
                response.headers.emplace_back("Allow", match.route->postBodyLimit ? "GET, HEAD, POST" : "GET, HEAD");
                hardenError();
            } else if (refusal) {
                response = Response::getStatusResponse(*refusal);
                hardenError();
            } else {
                co_await networking::ex::continues_on(routeWorker.run([&] {
                    reply = match.route->handler(RouteRequest{request, match.path, match.query, view});
                    routing::validateReply(reply);
                }),
                                                      Net::Scheduler(socket));
                response.statusCode = reply.status;
                for (auto& [name, value] : reply.headers)
                    response.headers.emplace_back(name, value);
                if (reply.contentLength)
                    response.headers.emplace_back("Content-Length", std::to_string(*reply.contentLength));
                else if (!reply.body)
                    response.headers.emplace_back("Content-Length", "0");
            }
        } catch (const std::exception& error) {
            if (match.route->secretPath)
                log::warn("Route {} failed (the error is not logged: it may quote a secret)", match.route->prefix);
            else
                log::warn("Route {} failed: {}", match.route->prefix, error.what());
            response = Response::getStatusResponse(Response::internalServerError);
            hardenError();
            reply.body = nullptr;
        } catch (...) {
            log::warn("Route {} failed with an exception which is not a std::exception", match.route->prefix);
            response = Response::getStatusResponse(Response::internalServerError);
            hardenError();
            reply.body = nullptr;
        }
        const bool streaming = !head && reply.body;  // the built-in 405 has no reply.body
        if (head)
            response.content.clear();                // an error page keeps its Content-Length; HEAD never has a body
        co_await Net::Send(socket, response.toBuffers<Net>());
        if (streaming)
            co_await streamBody(routeWorker, socket, std::move(reply.body), reply.contentLength, match.route->prefix, match.route->secretPath);
        co_await closeResponse(socket);
    }

    networking::Task serve(typename Net::Socket socket) {
        if constexpr (networking::secureTransport<Net>) {
            // The deadline and the server's shutdown cancel the handshake through this source.
            struct Forward {
                networking::ex::inplace_stop_source& source;
                void                                 operator()() noexcept {
                    source.request_stop();
                }
            };
            const auto                                                                       expired = std::make_shared<networking::ex::inplace_stop_source>();
            const auto                                                                       settled = std::make_shared<networking::ex::inplace_stop_source>();
            networking::ex::stop_callback_for_t<networking::ex::inplace_stop_token, Forward> shutdown(tasks.stopToken(), Forward{*expired});
            spawn(handshakeDeadline(expired, settled));
            struct Settle {
                networking::ex::inplace_stop_source& source;
                ~Settle() {
                    source.request_stop();
                }
            } settle{*settled};
            try {
                co_await networking::ex::stopped_as_error(
                    networking::ex::write_env(Net::Handshake(socket, acceptor), networking::ex::prop{networking::ex::get_stop_token, expired->get_token()}),
                    std::make_error_code(std::errc::timed_out));
            } catch (const std::system_error& error) {
                if (tasks.stopRequested())
                    co_return;  // interrupted by the server's shutdown, not a failed or expired handshake
                log::debug("TLS handshake failed ({}:{}): {}", error.code().category().name(), error.code().value(), error.what());
                co_return;
            } catch (const std::exception& error) {
                log::warn("TLS handshake failed: {}", error.what());
                co_return;
            }
        }
        Request                                                     request;
        std::optional<typename gzip::DecodeExecutor<Decode>::Lease> lease;
        std::unique_ptr<fs::File>                                   streamed;
        std::uint64_t                                               streamOffset = 0, streamLength = 0;
        bool                                                        head           = false;
        bool                                                        servicesTarget = false;
        Response                                                    response;
        std::string                                                 space;  // the path of the mount and segment, under a mount
        try {
            std::array<char, Policy::receptionBufferSize> buffer;
            std::string_view                              rest;  // received after the headers, still in `buffer`: the start of a body
            while (!request.completed()) {
                const auto received = co_await Net::Receive(socket, Net::Buffer(buffer));
                if (received == 0)
                    co_return;  // the client closed the connection
                const char* end  = buffer.data() + received;
                const char* stop = request.consume(static_cast<const char*>(buffer.data()), end);
                if (request.completed())
                    rest = std::string_view(stop, end);
            }
            std::shared_ptr<const RouteTable> spaceRoutes;
            if (auto mounted = resolveMount(request)) {
                if (mounted->reply) {
                    log::info("Received request {} under a mount: answered by its resolver ({})",
                              request.getMethodName(),
                              static_cast<int>(mounted->reply->status));
                    co_await sendReply(socket, request.method == Request::Method::Head, std::move(*mounted->reply));
                    co_return;
                }
                space       = std::move(mounted->space);
                spaceRoutes = std::move(mounted->routes);
                request.uri = std::move(mounted->uri);
            }
            const std::string_view within = space.empty() ? std::string_view{} : std::string_view(space).substr(0, space.size() - 1);
            auto                   match  = matchRoute(request, spaceRoutes.get());
            if (match && match->route->secretPath)
                log::info("Received request {} on {}{}...", request.getMethodName(), within, match->route->prefix);  // the rest is a secret
            else if (routing::mentionsServices(request.uri))
                log::info("Received request {} on a services path (not logged)", request.getMethodName());
            else
                log::info("Received request {} on {}{}",
                          request.getMethodName(),
                          within,
                          std::string_view(request.uri).substr(0, request.uri.find('?')));  // the query may quote a URL
            if (match) {
                co_await serveRoute(socket, std::move(request), std::move(*match), rest, buffer);
                co_return;
            }
            head           = request.method == Request::Method::Head;
            servicesTarget = routing::mentionsServices(request.uri);  // before the request is moved away
            auto prepared  = requestHandler.prepareRequest(std::move(request));
            if (prepared.needsDecode) {
                lease.emplace(co_await networking::ex::continues_on(decodeExecutor.submit(std::move(prepared.response.content), prepared.limit, prepared.head),
                                                                    Net::Scheduler(socket)));
                response = requestHandler.finish(std::move(prepared), *lease);
            } else {
                if (prepared.identityFallback && prepared.response.statusCode == Response::ok) {
                    lease.emplace(decodeExecutor.reserveResponse(prepared.response.content.capacity()));
                    if (!*lease) {
                        prepared.response = Response::getStatusResponse(Response::serviceUnavailable);
                        prepared.response.headers.emplace_back("Vary", "Accept-Encoding");
                    }
                }
                if (prepared.response.statusCode == Response::ok || prepared.response.statusCode == Response::partialContent) {
                    streamOffset = prepared.offset;
                    streamLength = prepared.length;
                    streamed     = std::move(prepared.file);
                }
                response = std::move(prepared.response);
            }
            log::info("  responding http {}", static_cast<int>(response.statusCode));
        } catch (const BadRequestException&) {
            response = Response::getStatusResponse(Response::badRequest);
        } catch (const std::bad_alloc&) {
            response = Response::getStatusResponse(Response::serviceUnavailable);
            response.headers.emplace_back("Vary", "Accept-Encoding");
        } catch (const std::exception& error) {
            if (servicesTarget)
                log::warn("HTTP asset request on a services path failed (the error is not logged)");
            else
                log::warn("HTTP asset request failed: {}", error.what());
            response = Response::getStatusResponse(Response::internalServerError);
        } catch (...) {
            log::warn("HTTP asset request failed with an exception which is not a std::exception");
            response = Response::getStatusResponse(Response::internalServerError);
        }
        if (head)
            response.content.clear();  // an error page keeps its Content-Length; HEAD never has a body
        co_await Net::Send(socket, response.toBuffers<Net>());
        if (streamed)
            co_await streamBody(
                fileWorker,
                socket,
                [&file = *streamed, offset = streamOffset, remaining = streamLength, positioned = streamOffset == 0](std::span<char> out) mutable {
                    if (!positioned) {
                        if (!file.seek(offset))
                            throw std::runtime_error("The file cannot seek");
                        positioned = true;
                    }
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), remaining));
                    if (count == 0)
                        return std::size_t{0};
                    const auto read = file.read(out.first(count));
                    remaining      -= read;
                    return read;
                },
                streamLength,
                "File response");
        if (response.statusCode != Response::switchingProtocols || tasks.stopRequested())
            co_await closeResponse(socket);  // a stopped server hands nothing over: its drain would never end
        else if (upgradeHandler)
            upgradeHandler(std::move(socket), space);
    }
};

}  // namespace webfront::http
