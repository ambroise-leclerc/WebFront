/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief HTTPServer minimal implementation for WebSocket support - RFC1945
#pragma once
#include <http/WebSocket.hpp>
#include <details/Encodings.hpp>
#include <details/HexDump.hpp>
#include <networking/BasicNetworking.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <locale>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace webfront {
namespace http {

struct Header {
    Header() = default;
    Header(std::string n, std::string v) : name(std::move(n)), value(std::move(v)) {}
    std::string name;
    std::string value;
};

struct Request {
    enum class Method { Connect, Delete, Get, Head, Options, Patch, Post, Put, Trace, Undefined };
    Method method;
    std::string uri;
    int httpVersionMajor;
    int httpVersionMinor;
    std::vector<Header> headers;

    void reset() {
        headers.clear();
        uri.clear();
    }

    constexpr Method getMethodFromString(std::string_view text) const {
        return text == "GET"       ? Method::Get
               : text == "HEAD"    ? Method::Head
               : text == "CONNECT" ? Method::Connect
               : text == "DELETE"  ? Method::Delete
               : text == "OPTIONS" ? Method::Options
               : text == "PATCH"   ? Method::Patch
               : text == "POST"    ? Method::Post
               : text == "PUT"     ? Method::Put
               : text == "TRACE"   ? Method::Trace
                                   : Method::Undefined;
    }

    void setMethod(std::string_view text) { method = getMethodFromString(text); }

    bool isUpgradeRequest(std::string_view protocol) const { return headerContains("Connection", "upgrade") && headerContains("Upgrade", protocol); }

    std::optional<std::string> getHeaderValue(std::string_view headerName) const {
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName)) return header.value;
        return {};
    }

private:
    /// @return true if text is contained in the value field of headerName (case insensitive)
    bool headerContains(std::string_view headerName, std::string_view text) const {
        auto header = getHeaderValue(headerName);
        if (header) {
            if (std::search(header.value().cbegin(), header.value().cend(), text.cbegin(), text.cend(),
                            [](char c1, char c2) { return (c1 == c2 || std::toupper(c1) == std::toupper(c2)); }) != header.value().cend())
                return true;
        }
        return false;
    }

    static constexpr bool caseInsensitiveEqual(std::string_view s1, std::string_view s2) {
        return ((s1.size() == s2.size()) &&
                std::equal(s1.begin(), s1.end(), s2.begin(), [](char c1, char c2) { return (c1 == c2 || std::toupper(c1) == std::toupper(c2)); }));
    }
};

struct Response {
    enum StatusCode : uint16_t { switchingProtocols = 101, ok = 200, badRequest = 400, notFound = 404, notImplemented = 501 };
    StatusCode statusCode;
    std::vector<Header> headers;
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
    std::vector<typename Net::ConstBuffer> toBuffers() const {
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
    static std::string toString(StatusCode code) {
        switch (code) {
        case switchingProtocols: return "Switching Protocols";
        case ok: return "OK";
        case badRequest: return "Bad Request";
        case notFound: return "Not Found";
        case notImplemented: return "Not Implemented";
        }
        return {};
    }
};

struct MimeType {
    enum Type { plain, html, css, js, jpg, png, gif };
    Type type;

    MimeType(Type mimeType) : type(mimeType) {}

    static MimeType fromExtension(std::string_view e) {
        if (e.starts_with('.')) e = e.substr(1);
        if (e == "htm" || e == "html") return html;
        if (e == "css") return css;
        if (e == "js" || e == "mjs") return js;
        if (e == "jpg" || e == "jpeg") return jpg;
        if (e == "png") return png;
        if (e == "gif") return gif;
        return plain;
    }

    std::string toString() const {
        std::string names[]{"text/plain", "text/html", "text/css", "application/javascript", "image/jpeg", "image/png", "image/gif"};
        return names[type];
    }
};

template<typename Net>
class RequestHandler {
public:
    RequestHandler(const RequestHandler&) = delete;
    RequestHandler& operator=(const RequestHandler&) = delete;

    explicit RequestHandler(std::filesystem::path root) : documentRoot(root) {}

    Response handleRequest(Request request) {
        auto requestUri = uri::decode(request.uri);
        if (requestUri.empty() || requestUri[0] != '/' || requestUri.find("..") != std::string::npos)
            return Response::getStatusResponse(Response::badRequest);
        auto requestPath = documentRoot / std::filesystem::path(requestUri).relative_path();
        if (!requestPath.has_filename()) requestPath /= "index.html";
        
        Response response;
        switch (request.method) {
        case Request::Method::Get:
            if (request.isUpgradeRequest("websocket")) {
                auto key = request.getHeaderValue("Sec-WebSocket-Key");
                if (key) {
                    response.statusCode = Response::StatusCode::switchingProtocols;
                    response.headers.emplace_back("Upgrade", "websocket");
                    response.headers.emplace_back("Connection", "Upgrade");
                    response.headers.emplace_back("Sec-WebSocket-Accept",
                                                  base64::encodeInNetworkOrder(crypto::sha1(key.value() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
                    response.headers.emplace_back("Sec-WebSocket-Protocol", "WebFront_0.1");
                    return response;
                }
            }
            [[fallthrough]];
        case Request::Method::Head: {
            std::ifstream file(requestPath, std::ios::in | std::ios::binary);
            if (!file) return Response::getStatusResponse(Response::notFound);

            if (request.method == Request::Method::Get) {
                char buffer[512];
                while (file.read(buffer, sizeof(buffer)).gcount() > 0)
                     response.content.append(buffer, static_cast<size_t>(file.gcount()));
            }
        } break;

        default: return Response::getStatusResponse(Response::notImplemented);
        };

        response.statusCode = Response::ok;
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", MimeType::fromExtension(requestPath.extension().string()).toString());

        return response;
    }

private:
    const std::filesystem::path documentRoot;
};

struct BadRequestException : public std::runtime_error {
    BadRequestException() : std::runtime_error("Bad HTTP request") {}
};

class RequestParser {
public:
    RequestParser() : state(State::methodStart) {}

    void reset() {
        currentRequest.reset();
        state = State::methodStart;
    }

    template<typename InputIterator>
    std::optional<Request> parse(InputIterator begin, InputIterator end) {
        while (begin != end)
            if (completeRequest(*begin++, currentRequest)) return currentRequest;

        return {};
    }

private:
    enum class State {
        methodStart,
        method,
        uri,
        versionH,
        versionT1,
        versionT2,
        versionP,
        versionSlash,
        versionMajorStart,
        versionMajor,
        versionMinorStart,
        versionMinor,
        expectingNewline1,
        headerLineStart,
        headerLws,
        headerName,
        spaceBeforeHeaderValue,
        headerValue,
        expectingNewline2,
        expectingNewline3
    };
    State state;
    Request currentRequest;

private: // clang-format off
    bool completeRequest(char input, Request& req) {
        auto isChar = [](char c) { return c >= 0; };
        auto isCtrl = [](char c) { return (c >= 0 && c <= 31) || (c == 127); };
        auto isSpecial = [](char c) {   switch (c) {
            case '(': case ')': case '<': case '>': case '@': case ',': case ';': case ':': case '\\': case '"':
            case '/': case '[': case ']': case '?': case '=': case '{': case '}': case ' ': case '\t': return true;
            default: return false;
        }};
        auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
        auto check = [this](bool cond, State next) {
            state = next;
            if (cond) throw BadRequestException();
        };

        static std::string buffer;

        switch (state) {
            case State::methodStart:
                check(!isChar(input) || isCtrl(input) || isSpecial(input), State::method);
                buffer = input;
                break;
            case State::method: if (input == ' ') { state = State::uri; req.setMethod(buffer); }
                              else if (!isChar(input) || isCtrl(input) || isSpecial(input)) { throw BadRequestException(); }
                              else buffer.push_back(input);
                break;
            case State::uri: if (input == ' ') { state = State::versionH; break; }
                           else if (isCtrl(input)) throw BadRequestException();
                           else { req.uri.push_back(input); break; }
            case State::versionH: check(input != 'H', State::versionT1); break;
            case State::versionT1: check(input != 'T', State::versionT2); break;
            case State::versionT2: check(input != 'T', State::versionP); break;
            case State::versionP: check(input != 'P', State::versionSlash); break;
            case State::versionSlash: check(input != '/', State::versionMajorStart); req.httpVersionMajor = 0; req.httpVersionMinor = 0; break;
            case State::versionMajorStart: check(!isDigit(input), State::versionMajor); req.httpVersionMajor = req.httpVersionMajor * 10 + input - '0'; break;
            case State::versionMajor: if (input == '.') state = State::versionMinorStart;
                                    else if (isDigit(input)) req.httpVersionMajor = req.httpVersionMajor * 10 + input - '0';
                                    else throw BadRequestException();
                break;
            case State::versionMinorStart:
                check(!isDigit(input), State::versionMinor);
                req.httpVersionMinor = req.httpVersionMinor * 10 + input - '0';
                break;
            case State::versionMinor: if (input == '\r') state = State::expectingNewline1;
                                    else if (isDigit(input)) req.httpVersionMinor = req.httpVersionMinor * 10 + input - '0';
                                    else throw BadRequestException();
                break;
            case State::expectingNewline1: check(input != '\n', State::headerLineStart); break;
            case State::headerLineStart: if (input == '\r') { state = State::expectingNewline3; break; }
                                       else if (!req.headers.empty() && (input == ' ' || input == '\t')) { state = State::headerLws; break; }
                                       else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                       else { req.headers.push_back(Header()); req.headers.back().name.push_back(input); state = State::headerName; break; }
            case State::headerLws:if (input == '\r') { state = State::expectingNewline2; break; }
                                 else if (input == ' ' || input == '\t') break;
                                 else if (isCtrl(input)) throw BadRequestException();
                                 else { state = State::headerValue; req.headers.back().value.push_back(input); break; }
            case State::headerName: if (input == ':') { state = State::spaceBeforeHeaderValue; break; }
                                  else if (!isChar(input) || isCtrl(input) || isSpecial(input)) throw BadRequestException();
                                  else { req.headers.back().name.push_back(input); break; }
            case State::spaceBeforeHeaderValue: check(input != ' ', State::headerValue); break;
            case State::headerValue: if (input == '\r') { state = State::expectingNewline2; break; }
                                   else if (isCtrl(input)) throw BadRequestException();
                                   else { req.headers.back().value.push_back(input); break; }
            case State::expectingNewline2: check(input != '\n', State::headerLineStart); break;
            case State::expectingNewline3: if (input == '\n') return true; else throw BadRequestException();
            default: throw BadRequestException();
        }
        return false;
    }
}; // clang-format on

template<typename Net>
class Connection : public std::enable_shared_from_this<Connection<Net>> {
public:
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    explicit Connection(typename Net::Socket sock, Connections<Connection>& connectionsHandler, RequestHandler<Net>& handler)
        : socket(std::move(sock)), connections(connectionsHandler), requestHandler(handler) {
            log::debug("New connection");
    }

    void start() { read(); }
    void stop() { socket.close(); }

public:
    std::function<void(typename Net::Socket)> onUpgrade;

private:
    typename Net::Socket socket;
    Connections<Connection<Net>>& connections;
    RequestHandler<Net>& requestHandler;
    std::array<char, 8192> buffer;
    RequestParser requestParser;
    Response response;
    enum class Protocol { HTTP, HTTPUpgrading, WebSocket };
    Protocol protocol = Protocol::HTTP;

    void read() {
        auto self(this->shared_from_this());
        socket.async_read_some(Net::Buffer(buffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
            if (!ec) {
                switch (protocol) {
                case Protocol::HTTP:
                    try {
                        auto request = requestParser.parse(buffer.data(), buffer.data() + bytesTransferred);
                        if (request) {
                            response = requestHandler.handleRequest(request.value());
                            write();
                            if (response.statusCode == Response::switchingProtocols) protocol = Protocol::HTTPUpgrading;
                        }
                        else
                            read();
                    }
                    catch (const BadRequestException&) {
                        response = Response::getStatusResponse(Response::badRequest);
                        write();
                    }
                    break;

                default: log::warn("Connection is no longer in HTTP protocol. Connection::read() is disabled.");
                }
            }
            else if (ec != Net::Error::OperationAborted)
                connections.stop(self);
        });
    }

    void write() {
        auto self(this->shared_from_this());
        Net::AsyncWrite(socket, response.toBuffers<Net>(), [this, self](std::error_code ec, std::size_t /*bytesTransferred*/) {
            if (protocol == Protocol::HTTPUpgrading) {
                protocol = Protocol::WebSocket;
                onUpgrade(std::move(socket));
            }
            else {
                if (!ec) socket.shutdown(Net::Socket::shutdown_both);
                if (ec != Net::Error::OperationAborted) connections.stop(self);
            }
        });
    }
};

template<typename Net>
requires networking::Features<Net>
class Server {
public:
    websocket::WSManager<Net> webSockets;

public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    explicit Server(std::string_view address, std::string_view port, std::filesystem::path docRoot = ".") : acceptor(ioContext), requestHandler(docRoot) {
        typename Net::Resolver resolver(ioContext);
        typename Net::Endpoint endpoint = *resolver.resolve(address, port).begin();
        acceptor.open(endpoint.protocol());
        acceptor.set_option(typename Net::Acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
        accept();
    }

    void run() { ioContext.run(); }
    void runOne() { ioContext.run_one(); }

private:
    typename Net::IoContext ioContext;
    typename Net::Acceptor acceptor;
    Connections<Connection<Net>> connections;
    RequestHandler<Net> requestHandler;

    void accept() {
        acceptor.async_accept([this](std::error_code ec, typename Net::Socket socket) {
            if (!acceptor.is_open()) return;
            auto newConnection = std::make_shared<Connection<Net>>(std::move(socket), connections, requestHandler);
            newConnection->onUpgrade = [this](typename Net::Socket sock) { webSockets.createWebSocket(std::move(sock)); };
            if (!ec) connections.start(newConnection);
            accept();
        });
    }

    void upgradeConnection(typename Net::Socket socket) { webSockets.createWebSocket(std::move(socket)); }
};

} // namespace http
} // namespace webfront