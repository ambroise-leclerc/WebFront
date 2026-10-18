/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief HTTPServer minimal implementation for WebSocket support - RFC1945
#pragma once
#include "../ApplicationOrigin.hpp"
#include "../ContentPolicy.hpp"
#include "../networking/BasicNetworking.hpp"
#include "../networking/Tasks.hpp"
#include "../system/FileSystem.hpp"
#include "../Task.hpp"
#include "../tooling/HexDump.hpp"
#include "BuffersPolicy.hpp"
#include "Encodings.hpp"
#include "MimeType.hpp"
#include "WebSocket.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <list>
#include <locale>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace webfront::http {

class Headers {
    struct Header {
        Header() = default;
        Header(std::string_view n, std::string_view v) : name(std::move(n)), value(std::move(v)) {}
        std::string name;
        std::string value;
    };

public:
    /// @return the header value with headerName name (or at least the first one)
    [[nodiscard]] std::optional<std::string> getHeaderValue(std::string_view headerName) const {
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName)) return header.value;
        return {};
    }

    /// @return the list of headers values with the same headerName name
    [[nodiscard]] std::list<std::string> getHeadersValues(std::string_view headerName) const {
        std::list<std::string> values{};
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName)) values.push_back(header.value);
        return values;
    }

    /// @return true if text is contained in the value field of headerName (case insensitive)
    [[nodiscard]] bool headersContain(std::string_view headerName, std::string_view text) const {
        for (auto header : getHeadersValues(headerName)) {
            if (std::search(header.cbegin(), header.cend(), text.cbegin(), text.cend(), [](char c1, char c2) {
                    return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
                }) != header.cend())
                return true;
        }
        return false;
    }

    std::vector<Header> headers;

private:
    [[nodiscard]] static constexpr bool caseInsensitiveEqual(std::string_view s1, std::string_view s2) {
        return ((s1.size() == s2.size()) && std::equal(s1.begin(), s1.end(), s2.begin(), [](char c1, char c2) {
                    return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
                }));
    }
};

struct BadRequestException : public std::runtime_error {
    BadRequestException() : std::runtime_error("Bad HTTP request") {}
};

struct Request : Headers {
    enum class Method { Connect, Delete, Get, Head, Options, Patch, Post, Put, Trace, Undefined };
    Method method;
    std::string uri;
    int httpVersionMajor;
    int httpVersionMinor;

    void reset() {
        headers.clear();
        uri.clear();
    }

    [[nodiscard]] static Method getMethodFromString(std::string_view text) {
        for (size_t index = 0; index < methodNames.size(); ++index)
            if (methodNames[index] == text) return static_cast<Method>(index);
        return Method::Undefined;
    }

    [[nodiscard]] std::string_view getMethodName() const { return method == Method::Undefined ? "Undefined" : methodNames[static_cast<size_t>(method)]; }
        
    void setMethod(std::string_view text) { method = getMethodFromString(text); }

    [[nodiscard]] bool isUpgradeRequest(std::string_view protocol) const {
        return headersContain("Connection", "upgrade") && headersContain("Upgrade", protocol);
    }

    template<typename InputIterator>
    bool parseSomeData(InputIterator begin, InputIterator end) {
        while (begin != end)
            if (completeRequest(*begin++)) return true;

        return false;
    }

    bool completed() const { return state == State::completed; }

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
                                 else { state = headerValue; headers.back().value.push_back(input); break; }
            case headerName: if (input == ':') { state = spaceBeforeHeaderValue; break; }
                                  else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                  else { headers.back().name.push_back(input); break; }
            case spaceBeforeHeaderValue: setState(input != ' ', headerValue); break;
            case headerValue: if (input == '\r') { state = newline2; break; }
                                   else if (isCtrl(input)) throw BadRequestException();
                                   else { headers.back().value.push_back(input); break; }
            case newline2: setState(input != '\n', headerLineStart); break;
            case newline3: if (input == '\n') { state = completed; return true; } else throw BadRequestException();
            case completed: return true;
            default: throw BadRequestException();
        }
        return false;
    }
    inline static std::array methodNames{"CONNECT", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT", "TRACE"};
}; // clang-format on

struct Response : Headers {
    enum StatusCode : uint16_t {
        switchingProtocols    = 101,
        ok                    = 200,
        badRequest            = 400,
        forbidden             = 403,
        notFound              = 404,
        notImplemented        = 501,
        variantAlsoNegotiates = 506
    };
    StatusCode statusCode;
    std::string content;

    static Response getStatusResponse(StatusCode code) {
        Response response;
        response.statusCode = code;
        response.content = "<html><head><title>" + toString(code) + "</title></head>";
        response.content += "<body><h1>" + std::to_string(code) + " " + toString(code) + "</h1></body></html>";
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", "text/html");

        return response;
    }

    template<typename Net>
    [[nodiscard]] std::vector<typename Net::ConstBuffer> toBuffers() const {
        std::vector<typename Net::ConstBuffer> buffers;
        static std::string httpStatus;
        httpStatus = "HTTP/1.1 " + std::to_string(statusCode) + " " + toString(statusCode) + "\r\n";
        buffers.push_back(Net::Buffer(httpStatus));

        static const char separator[] = {':', ' '};
        static const char crlf[] = {'\r', '\n'};
        for (auto& header : headers) {
            buffers.push_back(Net::Buffer(header.name));
            buffers.push_back(Net::Buffer(separator));
            buffers.push_back(Net::Buffer(header.value));
            buffers.push_back(Net::Buffer(crlf));
        }
        buffers.push_back(Net::Buffer(crlf));
        if (!content.empty()) buffers.push_back(Net::Buffer(content));

        return buffers;
    }

private:
    [[nodiscard]] static std::string toString(StatusCode code) {
        switch (code) {
        case switchingProtocols: return "Switching Protocols";
        case ok: return "OK";
        case badRequest: return "Bad Request";
        case forbidden:
            return "Forbidden";
        case notFound: return "Not Found";
        case notImplemented: return "Not Implemented";
        case variantAlsoNegotiates: return "Variant Also Negotiates";
        }
        return {};
    }
};

template<networking::Features Net, fs::Provider FS>
class RequestHandler {
public:
    explicit RequestHandler(std::filesystem::path root, detail::BoundContentPolicy policy = detail::BoundContentPolicy{})
        : fs(root), contentPolicy(std::move(policy)) { }
    ~RequestHandler() = default;
    RequestHandler(const RequestHandler&) = default;
    RequestHandler(RequestHandler&&) = default;
    RequestHandler& operator=(const RequestHandler&) = default;
    RequestHandler& operator=(RequestHandler&&) = default;

    Response handleRequest(Request request) {
        auto requestPath = filePath(request.uri);
        if (!requestPath) return Response::getStatusResponse(Response::badRequest);

        Response response;
        switch (request.method) {
        case Request::Method::Get:
            if (auto upgrade = webSocketUpgrade(request)) return *upgrade;
            if (auto error = readFile(request, *requestPath, response)) return *error;
            break;
        case Request::Method::Head:
            if (!fs.open(*requestPath)) return Response::getStatusResponse(Response::notFound);
            break;

        default: return Response::getStatusResponse(Response::notImplemented);
        };

        response.statusCode = Response::ok;
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", MimeType(requestPath->extension().string()).toString());

        return response;
    }

private:
    // The file a request URI names, or nothing when the URI is not a safe absolute path.
    static std::optional<std::filesystem::path> filePath(const std::string& uri) {
        // The query is not part of the file name; it is cut before decoding, so "%3F" stays in the name.
        const auto rawPath    = std::string_view(uri).substr(0, uri.find('?'));
        auto       requestUri = uri::decode(rawPath);
        log::debug("Request uri - raw:'{}' decoded path:'{}'", uri, requestUri);
        if (!isSafePath(requestUri)) return {};
        auto requestPath = std::filesystem::path(requestUri).relative_path();
        log::debug("  -> requestPath : {}", requestPath.string());
        if (!requestPath.has_filename()) requestPath /= "index.html";
        return requestPath;
    }

    static bool isSafePath(std::string_view path) {
        if (!path.starts_with('/')) return false;
        return path.find("..") == std::string_view::npos;
    }

    std::optional<Response> webSocketUpgrade(const Request& request) const {
        if (!request.isUpgradeRequest("websocket")) return {};
        const auto origins = request.getHeadersValues("Origin");
        if (origins.size() > 1 || !contentPolicy.allowsBridge(origins.empty() ? std::nullopt : std::optional<std::string_view>{origins.front()}))
            return rejectWebSocketOrigin();
        auto key = request.getHeaderValue("Sec-WebSocket-Key");
        if (!key) return {};
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
    std::optional<Response> readFile(const Request& request, const std::filesystem::path& requestPath, Response& response) {
        auto file = fs.open(requestPath);
        if (!file) return Response::getStatusResponse(Response::notFound);
        if (file->isEncoded() && !request.headersContain("Accept-Encoding", file->getEncoding())) {
            log::error("File {} encoding is not supported by client : HTTP ERROR 506", file->getEncoding());
            return Response::getStatusResponse(Response::variantAlsoNegotiates);
        }

        std::array<char, 512> buffer{0, 0};
        while (auto bytesRead = file->read(buffer)) response.content.append(buffer.data(), bytesRead);
        if (file->isEncoded()) response.headers.emplace_back("Content-Encoding", file->getEncoding());
        return {};
    }

    FS fs;
    detail::BoundContentPolicy contentPolicy;
};

template<networking::Features Net, fs::Provider FS, BuffersPolicyType Policy = DefaultBuffersPolicy>
class Server {
public:
    Server(std::string_view address, std::string_view port, std::filesystem::path docRoot = ".", ContentPolicy policy = {})
        : acceptor(ioContext), requestHandler(docRoot, detail::BoundContentPolicy{detail::applicationOrigin(address, port), std::move(policy)}) {
        const auto endpoint = Net::Resolve(address, port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(typename Net::Acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        spawn(acceptConnections());
    }
    ~Server() noexcept {
        drainUntilJoined();
    }
    Server(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&) = delete;

    void run() { ioContext.run(); }
    void runOne() { ioContext.run_one(); }

    /// Thread-safe. Cancels the acceptor and the connections, and makes run() return.
    void stop() {
        log::info("Stopping HTTP server...");
        tasks.requestStop();
        ioContext.stop();
    }

    /// Runs the cancelled tasks to completion, so that no connection outlives the objects its handlers use.
    /// To be called from the thread owning the context, once nothing else runs it.
    void drain() {
        tasks.drain(ioContext);
    }

    /** @brief Destructor-safe join, retaining ownership until every task has finished. */
    void drainUntilJoined() noexcept {
        tasks.drainUntilJoined(ioContext);
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

    /// Called, on the context's thread, with the socket of a connection upgraded to a WebSocket.
    void onUpgrade(std::function<void(typename Net::Socket&&)>&& handler) {
        upgradeHandler = std::move(handler);
    }

private:
    typename Net::IoContext                     ioContext;
    typename Net::Acceptor                      acceptor;
    networking::TaskScope                       tasks;
    RequestHandler<Net, FS> requestHandler;
    std::function<void(typename Net::Socket&&)> upgradeHandler;

    void spawn(networking::Task task) {
        networking::Spawn(tasks, Net::Scheduler(acceptor), std::move(task));
    }

    // A failed accept (an aborted connection, no file descriptor left...) must not end the accepting.
    networking::Task acceptConnections() {
        while (!tasks.stopRequested()) {
            try {
                spawn(serve(co_await Net::Accept(acceptor)));
            }
            catch (const std::system_error& error) {
                if (error.code() == std::errc::operation_canceled) co_return;
                log::error("Accept failed: {}", error.what());
            }
        }
    }

    networking::Task serve(typename Net::Socket socket) {
        Request request;
        Response response;
        try {
            std::array<char, Policy::receptionBufferSize> buffer;
            while (!request.completed()) {
                const auto received = co_await Net::Receive(socket, Net::Buffer(buffer));
                if (received == 0) co_return;  // the client closed the connection
                request.parseSomeData(buffer.data(), buffer.data() + received);
            }
            log::info("Received request {} on {}", request.getMethodName(), request.uri);
            response = requestHandler.handleRequest(request);
            log::info("  responding http {}", static_cast<int>(response.statusCode));
        }
        catch (const BadRequestException&) {
            response = Response::getStatusResponse(Response::badRequest);
        }
        co_await Net::Send(socket, response.toBuffers<Net>());
        if (response.statusCode != Response::switchingProtocols || tasks.stopRequested())
            socket.shutdown(Net::Socket::shutdown_both);  // a stopped server hands nothing over: its drain would never end
        else if (upgradeHandler)
            upgradeHandler(std::move(socket));
    }
};

} // namespace webfront::http
