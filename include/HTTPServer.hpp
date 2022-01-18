#pragma once
#include "details/Encodings.hpp"
#include "details/HexDump.hpp"
#include "WebSocket.hpp"

#include <algorithm>
#include <cstring>
#include <experimental/net>
#include <fstream>
#include <functional>
#include <locale>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <regex>

#include <iostream>

namespace webfront {
namespace http {
namespace net = std::experimental::net;

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    enum class Method { Connect, Delete, Get, Head, Options, Patch, Post, Put, Trace };
    Method method;
    std::string uri;
    int httpVersionMajor;
    int httpVersionMinor;
    std::vector<Header> headers;

    void reset() {
        headers.clear();
        uri.clear();
    }

    void setMethod(std::string text) {
        if (text == "CONNECT") method = Method::Connect;
        else if (text == "DELETE") method = Method::Delete;
        else if (text == "GET") method = Method::Get;
        else if (text == "HEAD") method = Method::Head;
        else if (text == "OPTIONS") method = Method::Options;
        else if (text == "PATCH") method = Method::Patch;
        else if (text == "POST") method = Method::Post;
        else if (text == "PUT") method = Method::Put;
        else if (text == "TRACE") method = Method::Trace;
    }

    bool isUpgradeRequest(std::string protocol) const {
        return headerContains("Connection", "upgrade") && headerContains("Upgrade", protocol);
    }

    std::optional<std::string> getHeaderValue(std::string headerName) const {
        for (auto& header : headers)
            if (caseInsensitiveEqual(header.name, headerName)) return header.value;
        return {};
    }

private:
    // return true if text is contained in the value field of headerName (case insensitive)
    bool headerContains(std::string headerName, std::string text) const {
        auto header = getHeaderValue(std::move(headerName));
        if (header) {
            if (std::search(header.value().cbegin(), header.value().cend(), text.cbegin(), text.cend(), [](char c1, char c2) {
                return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
            }) != header.value().cend()) return true;
        }
        return false;
    }

    static constexpr bool caseInsensitiveEqual(std::string_view s1, std::string_view s2) {
        return ((s1.size() == s2.size()) && std::equal(s1.begin(), s1.end(), s2.begin(), [](char c1, char c2) {
            return (c1 == c2 || std::toupper(c1) == std::toupper(c2));
        }));
    }
};

struct Response {
    enum StatusCode : uint16_t {
        switchingProtocols = 101, ok = 200, badRequest = 400, notFound = 404, notImplemented = 501
    };
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

    std::vector<net::const_buffer> toBuffers() const {
        std::vector<net::const_buffer> buffers;
        static std::string httpStatus;
        httpStatus = "HTTP/1.1 " + std::to_string(statusCode) + " " + toString(statusCode) + "\r\n";
        buffers.push_back(net::buffer(httpStatus));

        static const char separator[] = {':', ' '};
        static const char crlf[] = { '\r', '\n' };
        for (auto& header : headers) {
            buffers.push_back(net::buffer(header.name));
            buffers.push_back(net::buffer(separator));
            buffers.push_back(net::buffer(header.value));
            buffers.push_back(net::buffer(crlf));
        }
        buffers.push_back(net::buffer(crlf));
        buffers.push_back(net::buffer(content));

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

    MimeType(Type type) : type(type) {}

    static MimeType fromExtension(std::string e) {
        if (e == "htm" || e == "html") return html;
        if (e == "css") return css;
        if (e == "js" || e == "mjs") return js;
        if (e == "jpg" || e == "jpeg") return jpg;
        if (e == "png") return png;
        if (e == "gif") return gif;
        return plain;
    }

    std::string toString() const {
        std::string names[]{ "text/plain", "text/html", "text/css", "application/javascript", "image/jpeg", "image/png", "image/gif" };
        return names[type] ;
    }
};

class RequestHandler {
public:
    RequestHandler(const RequestHandler&) = delete;
    RequestHandler& operator=(const RequestHandler&) = delete;

    explicit RequestHandler(std::string documentRoot) : documentRoot(documentRoot) {}

    Response handleRequest(Request request) {
        Response response;
        auto requestPath = uri::decode(request.uri);
        if (requestPath.empty() || requestPath[0] != '/' || requestPath.find("..") != std::string::npos)
            return Response::getStatusResponse(Response::badRequest);
        if (requestPath[requestPath.size() - 1] == '/')
            requestPath += "index.html";
        auto lastSlash = requestPath.find_last_of("/");
        auto lastDot = requestPath.find_last_of(".");
        std::string extension;
        if (lastDot != std::string::npos && lastDot > lastSlash) extension = requestPath.substr(lastDot + 1);

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
                        std::cout << "-> Switch protocols\n";
                        return response;
                    }
                }
            case Request::Method::Head: {
                auto fullPath = documentRoot + requestPath;
                std::ifstream file(fullPath.c_str(), std::ios::in | std::ios::binary);
                if (!file) return Response::getStatusResponse(Response::notFound);

                if (request.method == Request::Method::Get) {
                    char buffer[512];
                    while (file.read(buffer, sizeof(buffer)).gcount() > 0)
                        response.content.append(buffer, file.gcount());
                }
            } break;

            default:
                return Response::getStatusResponse(Response::notImplemented);
        };
   
        response.statusCode = Response::ok;
        response.headers.emplace_back("Content-Length", std::to_string(response.content.size()));
        response.headers.emplace_back("Content-Type", MimeType::fromExtension(extension).toString());

        return response;
    }


private:
    std::string documentRoot;
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

    template <typename InputIterator>
    std::optional<Request> parse(InputIterator begin, InputIterator end) {
        while (begin != end)
            if (completeRequest(*begin++, currentRequest)) return currentRequest;
        
        return {};
    }


private:
    enum class State {
        methodStart, method, uri, versionH, versionT1, versionT2, versionP, versionSlash, versionMajorStart, versionMajor,
        versionMinorStart, versionMinor, expectingNewline1, headerLineStart, headerLws, headerName, spaceBeforeHeaderValue,
        headerValue, expectingNewline2, expectingNewline3
    };
    State state;
    Request currentRequest;

private:
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
};

template <typename ConnectionType>
class Connections {
public:
    Connections(const Connections&) = delete;
    Connections& operator=(const Connections&) = delete;
    Connections() = default;

    void start(std::shared_ptr<ConnectionType> connection) {
        connections.insert(connection);
        connection->start();
    }

    void stop(std::shared_ptr<ConnectionType> connection) {
        connections.erase(connection);
        connection->stop();
    }

    void stopAll() {
        for (auto connection : connections)
            connection->stop();
        connections.clear();
    }

private:
    std::set<std::shared_ptr<ConnectionType>> connections;
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    explicit Connection(net::ip::tcp::socket socket, Connections<Connection>& connections, RequestHandler& handler)
        : socket(std::move(socket)), connections(connections), requestHandler(handler) { 
        std::clog << "New connection\n";
    }

    void start() { read();}
    void stop() { socket.close(); }

public:
    std::function<void(net::ip::tcp::socket)> onUpgrade;

private:
    net::ip::tcp::socket socket;
    Connections<Connection>& connections;
    RequestHandler& requestHandler;
    std::array<char, 8192> buffer;
    RequestParser requestParser;
    Response response;
    enum class Protocol { HTTP, WebSocket };
    Protocol protocol = Protocol::HTTP;

    void read() {
        auto self(shared_from_this());
        socket.async_read_some(net::buffer(buffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
            if (!ec) {
              // std::cout << "Received : " << bytesTransferred << " bytes \n" << utils::HexDump(std::span(reinterpret_cast<const uint8_t*>(buffer.data()), bytesTransferred)) << "\n";

                switch (protocol) {
                    case Protocol::HTTP:
                        try {
                            auto request = requestParser.parse(buffer.data(), buffer.data() + bytesTransferred);
                            if (request) {
                                response = requestHandler.handleRequest(request.value());
                                write();
                                if (response.statusCode == Response::switchingProtocols) {
                                    protocol = Protocol::WebSocket;
                                    onUpgrade(std::move(socket));
                                }
                            }
                            else read();
                        }
                        catch (const BadRequestException&) {
                            response = Response::getStatusResponse(Response::badRequest);
                            write();
                        }
                        break;

                    default: std::clog << "Connection is no longer in HTTP protocol. Connection::read() is disabled.\n";
                }
            }
            else if (ec != net::error::operation_aborted)
                connections.stop(shared_from_this());
        });
    }

    void write() {
        auto self(shared_from_this());
        switch (protocol) {
            case Protocol::HTTP: {
                net::async_write(socket, response.toBuffers(), [this, self](std::error_code ec, std::size_t /*bytesTransferred*/) {
                    if (!ec) socket.shutdown(net::ip::tcp::socket::shutdown_both);
                    if (ec != net::error::operation_aborted) connections.stop(shared_from_this());
                });
            } break;
            default: std::clog << "Connection is no longer in HTTP protocol. Connection::write() is disabled.\n";
        }
    }
};

class Server {
public:
    websocket::WSManager<net::ip::tcp::socket> webSockets;
public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    explicit Server(std::string_view address, std::string_view port)
        : acceptor(ioContext), requestHandler(".") {

        net::ip::tcp::resolver resolver(ioContext);
        net::ip::tcp::endpoint endpoint = *resolver.resolve(address, port).begin();
        acceptor.open(endpoint.protocol());
        acceptor.set_option(net::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        accept();
    }

    void run() { ioContext.run(); }
    void runOne() { ioContext.run_one(); }

private:
    net::io_context ioContext;
    net::ip::tcp::acceptor acceptor;
    Connections<Connection> connections;
    RequestHandler requestHandler;

    void accept() {
        acceptor.async_accept([this](std::error_code ec, net::ip::tcp::socket socket) {
            if (!acceptor.is_open()) return;
            auto newConnection = std::make_shared<Connection>(std::move(socket), connections, requestHandler);
            newConnection->onUpgrade = [this](net::ip::tcp::socket socket) {
                webSockets.createWebSocket(std::move(socket));
                std::cout << "web socket !\n";
            };
            if (!ec) connections.start(newConnection);
            accept();
        });
    }
};

} // namespace http
} // namespace webfront