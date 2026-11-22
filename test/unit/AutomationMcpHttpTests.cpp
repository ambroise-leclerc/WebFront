// MCP over Streamable HTTP served by the application (ADR-025): header screening, MCP sessions, the loopback listener
// on real sockets and its activation through enableAutomation.
#include "TemporaryDirectory.hpp"
#include "Bdd.hpp"
#include <webfront/automation/Enable.hpp>
#include <webfront/automation/McpHttp.hpp>

#include <deque>
#include <fstream>
#include <sstream>
#ifndef _WIN32
    #include <sys/stat.h>
#endif

using namespace webfront::bdd;

namespace mcphttptests {
using namespace webfront::automation;

webfront::http::Request parsed(std::string_view text) {
    webfront::http::Request request;
    request.consume(text.data(), text.data() + text.size());
    require(request.completed());
    return request;
}

const std::string token(64, 'a');

std::string post(unsigned short port, std::string_view body, std::string_view extra = {}, std::string_view bearer = token) {
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nAuthorization: Bearer " + std::string(bearer)
         + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: " + std::to_string(body.size())
         + "\r\n" + std::string(extra) + "\r\n" + std::string(body);
}

int status(const mcphttp::Screening& screening) {
    return screening.refusal ? screening.refusal->status : 0;
}

struct Reply {
    int         status = 0;
    std::string head, body;
    std::string header(std::string_view name) const {
        std::istringstream lines{head};
        std::string        line;
        while (std::getline(lines, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = line.substr(0, colon);
            std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string wanted{name};
            std::ranges::transform(wanted, wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key == wanted) {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' '))
                    value.erase(0, 1);
                while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
                    value.pop_back();
                return value;
            }
        }
        return {};
    }
};

// Reads until the peer closes its side, or a deadline.
std::string drain(channel::Handle socket, std::string text = {}, std::string_view until = {}) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        if (!until.empty() && text.find(until) != std::string::npos)
            return text;
        if (!channel::wait(socket, POLLIN, 50))
            continue;
        char       bytes[4096];
        const auto count = channel::SocketRead{}(socket, bytes, sizeof bytes);
        if (count <= 0)
            return text;
        text.append(bytes, static_cast<std::size_t>(count));
    }
    return text;
}

Reply reply(std::string text) {
    while (text.starts_with("HTTP/1.1 100"))
        text.erase(0, text.find("\r\n\r\n") + 4);
    Reply result;
    const auto end = text.find("\r\n\r\n");
    require(end != std::string::npos);
    result.head   = text.substr(0, end);
    result.body   = text.substr(end + 4);
    result.status = std::stoi(text.substr(9, 3));
    return result;
}

void connectTo(const channel::Socket& peer, unsigned short port) {
    const auto address = channel::address(port);
    require(::connect(peer.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
}

Reply roundTrip(unsigned short port, std::string_view bytes) {
    [[maybe_unused]] channel::Runtime runtime;
    channel::Socket                   peer{channel::createSocket()};
    connectTo(peer, port);
    channel::send(peer.value, bytes);
    return reply(drain(peer.value));
}

std::string bearer(const std::filesystem::path& directory) {
    const Object configuration{channel::readEndpoint(directory, mcphttp::configurationFile)};
    const Object server{Object(configuration.raw("mcpServers")).raw("webfront")};
    require(server.string("type") == "http");
    const auto authorization = Object(server.raw("headers")).string("Authorization");
    require(authorization.starts_with("Bearer "));
    return authorization.substr(7);
}

std::string url(const std::filesystem::path& directory) {
    const Object configuration{channel::readEndpoint(directory, mcphttp::configurationFile)};
    return Object(Object(configuration.raw("mcpServers")).raw("webfront")).string("url");
}

constexpr std::string_view initialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";
constexpr std::string_view initialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
constexpr std::string_view listTools   = R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})";

struct HttpConsumerApp {
    struct UI {
        int  value;
        int  id() const { return value; }
        void addScript(std::string) const { }
        friend bool operator==(const UI& a, const UI& b) { return a.value == b.value; }
    };
    std::filesystem::path    project;
    std::vector<std::string> registered{};
    template <typename Handler>
    void cppFunction(std::string_view name, Handler) {
        registered.emplace_back(name);
    }
    webfront::UISubscription subscribeUIStarted(std::function<void(UI)>) { return {}; }
    webfront::UISubscription subscribeUIClosed(std::function<void(UI)>) { return {}; }
    const std::filesystem::path& projectDirectory() const { return project; }
};
}  // namespace mcphttptests

const Scenario mcpHttpScreening{"MCP HTTP requests are screened on their headers before any body is read", [] {
    using namespace mcphttptests;
    constexpr unsigned short port = 4321;
    const auto               screened = [&](std::string_view text) { return mcphttp::screen(parsed(text), port, token); };
    given("a well-formed authenticated POST", [&] {
        then("it is accepted with its announced length", [&] {
            const auto accepted = screened(post(port, "{}"));
            require(!accepted.refusal);
            require(accepted.length == 2);
            require(!accepted.expectsContinue);
            require(screened(post(port, "{}", "Expect: 100-continue\r\n")).expectsContinue);
        });
        then("localhost is the other accepted spelling, and the Bearer scheme is case-insensitive", [&] {
            auto text = post(port, "{}");
            text.replace(text.find("127.0.0.1"), 9, "LocalHost");
            require(!screened(text).refusal);
            text.replace(text.find("Bearer"), 6, "bearer");
            require(!screened(text).refusal);
        });
    });
    given("requests a web page could send", [&] {
        then("a foreign Host, as DNS rebinding produces, is refused with 421, even with the token", [&] {
            auto text = post(port, "{}");
            text.replace(text.find("127.0.0.1"), 9, "attacker.example");
            require(status(screened(text)) == 421);
            auto other = post(port, "{}");
            other.replace(other.find(":4321"), 5, ":4322");
            require(status(screened(other)) == 421);
            require(status(screened("POST /mcp HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\n\r\n")) == 421);
            require(status(screened(post(port, "{}", "Host: 127.0.0.1:4321\r\n"))) == 421);  // repeated
        });
        then("any Origin, the application's own included, and any Sec-Fetch header are refused with 403, even with the token", [&] {
            require(status(screened(post(port, "{}", "Origin: http://127.0.0.1:9002\r\n"))) == 403);
            require(status(screened(post(port, "{}", "Origin: null\r\n"))) == 403);
            require(status(screened(post(port, "{}", "Sec-Fetch-Site: same-origin\r\n"))) == 403);
            require(status(screened(post(port, "{}", "Sec-Fetch-Mode: no-cors\r\n"))) == 403);
        });
        then("a CORS preflight and a GET get 405 and no Access-Control header", [&] {
            const auto preflight = screened("OPTIONS /mcp HTTP/1.1\r\nHost: 127.0.0.1:4321\r\n\r\n");
            require(status(preflight) == 405);
            const auto serialized = preflight.refusal->serialize();
            require(serialized.find("Access-Control") == std::string::npos);
            require(serialized.find("Allow: POST, DELETE") != std::string::npos);
            require(status(screened("GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:4321\r\nAuthorization: Bearer " + token + "\r\n\r\n")) == 405);
        });
        then("a form-like body type is refused with 415", [&] {
            auto text = post(port, "{}");
            text.replace(text.find("application/json\r\n"), 16, "text/plain");
            require(status(screened(text)) == 415);
            auto charset = post(port, "{}");
            charset.replace(charset.find("application/json\r\n"), 16, "Application/JSON; charset=utf-8");
            require(!screened(charset).refusal);
        });
    });
    given("other paths", [&] {
        then("only /mcp is served: the page's files and the OAuth discovery paths are 404", [&] {
            for (const auto* path : {"/WebFront.js", "/", "/mcp/", "/mcp?x=1", "/.well-known/oauth-protected-resource", "/.well-known/oauth-authorization-server"}) {
                auto text = post(port, "{}");
                text.replace(text.find("/mcp"), 4, path);
                require(status(screened(text)) == 404);
            }
        });
    });
    given("requests of a native client", [&] {
        then("a missing, malformed or wrong token is refused with 401 and WWW-Authenticate: Bearer", [&] {
            auto missing = post(port, "{}");
            missing.erase(missing.find("Authorization"), missing.find("\r\n", missing.find("Authorization")) + 2 - missing.find("Authorization"));
            const auto refused = screened(missing);
            require(status(refused) == 401);
            require(refused.refusal->serialize().find("WWW-Authenticate: Bearer\r\n") != std::string::npos);
            require(status(screened(post(port, "{}", {}, std::string(64, 'b')))) == 401);
            require(status(screened(post(port, "{}", {}, token.substr(1)))) == 401);
            auto basic = post(port, "{}");
            basic.replace(basic.find("Bearer"), 6, "Basic");
            require(status(screened(basic)) == 401);
            require(status(screened(post(port, "{}", "Authorization: Bearer " + token + "\r\n"))) == 401);  // repeated
        });
        then("an Accept without application/json is 406, a look-alike or a zero quality included", [&] {
            const auto accepting = [&](std::string_view accept) {
                auto text = post(port, "{}");
                text.replace(text.find("application/json, text/event-stream"), 35, accept);
                return status(screened(text));
            };
            require(accepting("text/event-stream") == 406);
            require(accepting("application/jsonp") == 406);
            require(accepting("application/json;q=0") == 406);
            require(accepting("application/json; q=0.000, text/event-stream") == 406);
            require(accepting("text/event-stream, Application/JSON;q=0.5") == 0);
            require(accepting("application/*") == 0);
            require(accepting("*/*;q=1") == 0);
        });
        then("chunked, unannounced, malformed and oversized bodies are refused before they are read", [&] {
            require(status(screened(post(port, "{}", "Transfer-Encoding: chunked\r\n"))) == 501);
            auto unannounced = post(port, "");
            unannounced.erase(unannounced.find("Content-Length"), std::string_view("Content-Length: 0\r\n").size());
            require(status(screened(unannounced)) == 411);
            auto malformed = post(port, "{}");
            malformed.replace(malformed.find("Content-Length: 2"), 17, "Content-Length: x2");
            require(status(screened(malformed)) == 400);
            auto oversized = post(port, "{}");
            oversized.replace(oversized.find("Content-Length: 2"), 17, "Content-Length: " + std::to_string(messageLimit + 1));
            require(status(screened(oversized)) == 413);
        });
        then("refusals quote nothing of the request", [&] {
            const auto refused = screened(post(port, "{}", {}, "secret-not-the-token"));
            require(refused.refusal->serialize().find("secret") == std::string::npos);
        });
    });
}};

const Scenario mcpHttpSessions{"MCP HTTP sessions follow the Streamable HTTP rules over the shared MCP profile", [] {
    using namespace mcphttptests;
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    auto controller = std::make_shared<Session>(SessionOptions{root.path() / "project", {"build"}});
    controller->poll();
    mcphttp::Endpoint<Session> endpoint{*controller};
    constexpr unsigned short   port   = 4321;
    const auto                 handle = [&](std::string_view body, std::string_view extra = {}) {
        return endpoint.handle(parsed(post(port, body, extra)), body);
    };
    given("a request without a session", [&] {
        then("anything but initialize is refused with 400", [&] {
            require(handle(listTools).status == 400);
            require(handle(initialized).status == 400);
            require(endpoint.sessions() == 0);
        });
        then("a batch and a malformed body are refused with 400", [&] {
            require(handle(std::string("[") + std::string(initialize) + "]").status == 400);
            const auto broken = handle("{\"jsonrpc\":");
            require(broken.status == 400);
            require(Object(Object(broken.body).raw("error")).raw("code") == "-32700");
        });
    });
    given("an initialized session", [&] {
        const auto opened = handle(initialize);
        require(opened.status == 200);
        const auto identifier = [&] {
            for (const auto& [name, value] : opened.headers)
                if (name == "Mcp-Session-Id")
                    return value;
            return std::string{};
        }();
        const auto withSession = "Mcp-Session-Id: " + identifier + "\r\n";
        then("the answer names the session, the negotiated revision and instructions", [&] {
            require(identifier.size() == 32);
            const Object result{Object(opened.body).raw("result")};
            require(result.string("protocolVersion") == "2025-11-25");
            require(result.string("instructions").find("webfront.session") != std::string::npos);
        });
        then("a notification is accepted with 202 and no body, and the tools are listed", [&] {
            const auto notified = handle(initialized, withSession);
            require(notified.status == 202);
            require(notified.body.empty());
            const auto tools = handle(listTools, withSession + "MCP-Protocol-Version: 2025-11-25\r\n");
            require(tools.status == 200);
            for (const auto* name : {"webfront.session", "webfront.start", "webfront.result", "webfront.cancel"})
                require(tools.body.find(name) != std::string::npos);
            const auto described = handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"webfront.session","arguments":{}}})", withSession);
            require(described.body.find(Object(controller->describe()).string("session")) != std::string::npos);
        });
        then("a client response is accepted with 202, and anything else without a method is refused with 400", [&] {
            require(handle(R"({"jsonrpc":"2.0","id":7,"result":{}})", withSession).status == 202);
            require(handle(R"({"jsonrpc":"2.0","id":7,"error":{"code":1,"message":"no"}})", withSession).status == 202);
            require(handle("{}", withSession).status == 400);
            require(handle(R"({"jsonrpc":"2.0","id":7})", withSession).status == 400);
            require(handle(R"({"jsonrpc":"2.0","result":{}})", withSession).status == 400);
            require(handle(R"({"jsonrpc":"2.0","id":7,"result":{},"error":{}})", withSession).status == 400);
            require(handle(R"({"jsonrpc":"1.0","id":7,"result":{}})", withSession).status == 400);
        });
        then("an unsupported MCP-Protocol-Version is refused with 400, for a response and a termination too", [&] {
            const auto old = withSession + "MCP-Protocol-Version: 2024-11-05\r\n";
            require(handle(listTools, old).status == 400);
            require(handle(R"({"jsonrpc":"2.0","id":7,"result":{}})", old).status == 400);
            auto removal = post(port, "");
            removal.replace(0, 4, "DELETE");
            removal.insert(removal.find("\r\n\r\n") + 2, old);
            require(endpoint.handle(parsed(removal), {}).status == 400);
            require(handle(listTools, withSession).status == 200);
        });
        then("an unknown session is 404, and DELETE terminates the session only", [&] {
            require(handle(listTools, "Mcp-Session-Id: " + std::string(32, '0') + "\r\n").status == 404);
            auto removal = post(port, "");
            removal.replace(0, 4, "DELETE");
            removal.insert(removal.find("\r\n\r\n") + 2, withSession);
            const auto terminated = endpoint.handle(parsed(removal), {});
            require(terminated.status == 204);
            require(terminated.serialize().find("Content-Length") == std::string::npos);  // forbidden on 204
            require(handle(listTools, withSession).status == 404);
            require(endpoint.handle(parsed(removal), {}).status == 404);
            require(!controller->isClosed());
        });
    });
    given("an initialize request whose id makes the answer larger than the request limit", [&] {
        const auto id   = std::string(messageLimit - 300, 'i');
        const auto body = R"({"jsonrpc":"2.0","id":")" + id + R"(","method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";
        then("the session is still created", [&] {
            require(body.size() <= messageLimit);
            const auto opened = handle(body);
            require(opened.status == 200);
            require(opened.body.size() > messageLimit);
            require(endpoint.sessions() == 1);
        });
    });
    given("more sessions than the bound", [&] {
        std::vector<std::string> identifiers;
        for (std::size_t index = 0; index < mcphttp::maxSessions; ++index) {
            const auto opened = handle(initialize);
            require(opened.status == 200);
            identifiers.push_back(opened.headers.front().second);
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        then("the least recently used are evicted, and a refused message does not count as activity", [&] {
            const auto oldest = "Mcp-Session-Id: " + identifiers.front() + "\r\n";
            require(handle(listTools, oldest + "MCP-Protocol-Version: 2024-11-05\r\n").status == 400);
            require(handle(initialize).status == 200);
            require(endpoint.sessions() == mcphttp::maxSessions);
            require(handle(listTools, oldest).status == 404);
            for (std::size_t index = 0; index < 4; ++index)
                require(handle(initialize).status == 200);
            require(endpoint.sessions() == mcphttp::maxSessions);
        });
    });
}};

const Scenario mcpHttpListener{"The MCP listener serves loopback HTTP with a private client configuration", [] {
    using namespace mcphttptests;
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    auto controller = std::make_shared<Session>(SessionOptions{root.path() / "project", {"build"}});
    controller->poll();
    channel::PrivateDirectory  directory{root.path() / "private"};
    mcphttp::Server<Session>   server{controller, directory.path};
    const auto                 port   = server.port();
    const auto                 secret = bearer(directory.path);
    given("the published client configuration", [&] {
        then("it is private, names the loopback URL and holds a 256-bit token that no other file shows", [&] {
            require(server.url() == "http://127.0.0.1:" + std::to_string(port) + "/mcp");
            require(url(directory.path) == server.url());
            require(secret.size() == 64);
#ifndef _WIN32
            struct stat info{};
            require(::stat(server.configuration().c_str(), &info) == 0);
            require((info.st_mode & 0777) == 0600);
#endif
        });
    });
    given("an unauthenticated client that sends its body at once", [&] {
        then("it reads its 401 instead of a reset", [&] {
            const auto refused = roundTrip(port, post(port, initialize, {}, std::string(64, 'b')));
            require(refused.status == 401);
            require(refused.header("WWW-Authenticate") == "Bearer");
            require(refused.header("Connection") == "close");
        });
        then("a page's request is refused even with the token", [&] {
            require(roundTrip(port, post(port, initialize, "Origin: http://127.0.0.1:9002\r\n", secret)).status == 403);
        });
    });
    given("an authenticated client", [&] {
        then("it initializes, lists the tools and starts an operation through its session", [&] {
            const auto opened = roundTrip(port, post(port, initialize, {}, secret));
            require(opened.status == 200);
            require(opened.header("Content-Type") == "application/json");
            const auto session = "Mcp-Session-Id: " + opened.header("Mcp-Session-Id") + "\r\nMCP-Protocol-Version: 2025-11-25\r\n";
            require(roundTrip(port, post(port, initialized, session, secret)).status == 202);
            require(roundTrip(port, post(port, listTools, session, secret)).body.find("webfront.start") != std::string::npos);
            const auto id      = Object(controller->describe()).string("session");
            const auto started = roundTrip(port, post(port, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"webfront.start","arguments":{"session":")" + id + R"(","operation":"build"}}})", session, secret));
            require(started.status == 200);
            const Object result{Object(started.body).raw("result")};
            require(result.raw("isError") == "false");
            require(!Object(result.raw("structuredContent")).string("runId").empty());
            const auto refused = roundTrip(port, post(port, R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"webfront.start","arguments":{"session":")" + id + R"(","operation":"test"}}})", session, secret));
            require(Object(Object(refused.body).raw("result")).raw("isError") == "true");
        });
        then("Expect: 100-continue receives the interim answer before the body is sent", [&] {
            [[maybe_unused]] channel::Runtime runtime;
            const auto                        text = post(port, initialize, "Expect: 100-continue\r\n", secret);
            const auto                        head = text.substr(0, text.find("\r\n\r\n") + 4);
            channel::Socket                   peer{channel::createSocket()};
            connectTo(peer, port);
            channel::send(peer.value, head);
            const auto interim = drain(peer.value, {}, "\r\n\r\n");
            require(interim.starts_with("HTTP/1.1 100 Continue\r\n\r\n"));
            channel::send(peer.value, text.substr(head.size()));
            require(reply(drain(peer.value, interim)).status == 200);
        });
        then("an unauthenticated client never receives 100 Continue: its refusal comes first", [&] {
            [[maybe_unused]] channel::Runtime runtime;
            const auto                        text = post(port, initialize, "Expect: 100-continue\r\n", std::string(64, 'c'));
            channel::Socket                   peer{channel::createSocket()};
            connectTo(peer, port);
            channel::send(peer.value, text.substr(0, text.find("\r\n\r\n") + 4));
            require(reply(drain(peer.value)).status == 401);
        });
    });
    given("silent peers that connect and send nothing", [&] {
        [[maybe_unused]] channel::Runtime runtime;
        std::deque<channel::Socket>       connected;  // Socket is neither copyable nor movable
        for (int index = 0; index < 80; ++index) {
            connected.emplace_back(channel::createSocket());
            const auto address = channel::address(port);
            require(::connect(connected.back().value, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        }
        then("an authenticated request is answered well before their two-second deadline", [&] {
            const auto started = std::chrono::steady_clock::now();
            require(roundTrip(port, post(port, initialize, {}, secret)).status == 200);
            require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{1500});
        });
    });
    given("a chosen port that is taken", [&] {
        then("the activation fails and names the address, the port and the cause", [&] {
            channel::PrivateDirectory other{root.path() / "other"};
            try {
                mcphttp::Server<Session> second{controller, other.path, port};
                require(false);
            } catch (const std::system_error& error) {
                require(std::string(error.what()).find("127.0.0.1:" + std::to_string(port)) != std::string::npos);
            }
            require(!std::filesystem::exists(other.path / mcphttp::configurationFile));
        });
    });
    given("the session closes", [&] {
        controller->close("Developer window closed");
        then("the listener closes and the configuration with its token is removed", [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (std::filesystem::exists(server.configuration()) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            require(!std::filesystem::exists(server.configuration()));
            [[maybe_unused]] channel::Runtime runtime;
            channel::Socket                   peer{channel::createSocket()};
            const auto                        address = channel::address(port);
            require(::connect(peer.value, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0);
        });
    });
}};

const Scenario mcpHttpActivation{"An application that enables automation serves its own MCP URL", [] {
    using namespace mcphttptests;
    webfront::test::TemporaryDirectory root;
    std::filesystem::create_directory(root.path() / "project");
    std::filesystem::create_directory(root.path() / "project2");
    HttpConsumerApp first{.project = root.path() / "project"}, second{.project = root.path() / "project2"};
    const auto      sessions = root.path() / "sessions";
    given("two instances enabled with no private directory chosen", [&] {
        auto one = enableAutomation(first, {.grants = {"check"}, .discovery = sessions, .announce = false});
        auto two = enableAutomation(second, {.grants = {"build"}, .discovery = sessions, .announce = false});
        then("each has its own URL and private configuration, under a directory named after its session", [&] {
            require(one->mcpUrl() != two->mcpUrl());
            require(one->mcpUrl().starts_with("http://127.0.0.1:"));
            require(one->clientConfiguration().parent_path() == one->directory());
            require(one->directory().filename() == Object(one->controller()->describe()).string("session"));
            require(bearer(one->directory()) != bearer(two->directory()));
        });
        then("the records list both instances with their URLs, never their tokens", [&] {
            const auto listed = discovery::list(sessions);
            require(listed.size() == 2);
            for (const auto& entry : listed) {
                require(!entry.mcp.empty());
                require(std::filesystem::exists(entry.configuration));
                std::ifstream      input(sessions / (entry.session + ".json"));
                std::ostringstream text;
                text << input.rdbuf();
                require(text.str().find(bearer(entry.endpoint)) == std::string::npos);
            }
        });
        then("the banner gives the URL and the configuration path, not the token", [&] {
            const auto banner = one->banner();
            require(banner.find(one->mcpUrl()) != std::string::npos);
            require(banner.find("mcp.json") != std::string::npos);
            require(banner.find(bearer(one->directory())) == std::string::npos);
        });
        then("each URL answers with its own session only", [&] {
            const auto port   = static_cast<unsigned short>(std::stoi(one->mcpUrl().substr(one->mcpUrl().rfind(':') + 1)));
            const auto opened = roundTrip(port, post(port, initialize, {}, bearer(one->directory())));
            require(opened.status == 200);
            require(roundTrip(port, post(port, initialize, {}, bearer(two->directory()))).status == 401);
        });
        then("closing withdraws the record, the configuration and the private directory", [&] {
            const auto directory = one->directory();
            one->close();
            require(!std::filesystem::exists(directory));
            require(discovery::list(sessions).size() == 1);
        });
    });
    given("a record of a vanished instance", [&] {
        const auto dead = std::string(64, 'd');
        discovery::preparePrivateDirectory(sessions);
        std::filesystem::create_directory(sessions / dead);
        // The record holds the resolved path, as channel::outsideProject gives it (the temporary root may be a link, as on macOS).
        const auto       own = std::filesystem::weakly_canonical(sessions / dead);
        discovery::Entry entry{.session = dead, .project = "/work/project", .endpoint = own.generic_string(), .allow = "[]", .pid = 2147483000L,
                               .mcp = "http://127.0.0.1:1/mcp", .configuration = (own / "mcp.json").generic_string()};
        std::ofstream(sessions / dead / "mcp.json") << "{}";
        channel::writeEndpoint(sessions / (dead + ".json"), entry.json());
        then("listing removes the record with its own directory and known files", [&] {
            require(discovery::list(sessions).empty());
            require(!std::filesystem::exists(sessions / (dead + ".json")));
            require(!std::filesystem::exists(sessions / dead));
        });
        then("the next activation removes them too, before it publishes its own", [&] {
            auto next = enableAutomation(first, {.grants = {"check"}, .discovery = sessions, .announce = false});
            require(!std::filesystem::exists(sessions / (dead + ".json")));
            require(!std::filesystem::exists(sessions / dead));
            require(discovery::list(sessions).size() == 1);
        });
    });
#ifndef _WIN32
    given("a vanished instance whose discovery directory is reached through a link", [&] {
        const auto dead  = std::string(64, 'e');
        const auto real  = root.path() / "real";
        const auto alias = root.path() / "alias";
        std::filesystem::create_directory(real);
        std::filesystem::create_directory_symlink(real, alias);
        const auto linked = alias / "sessions";
        discovery::preparePrivateDirectory(linked);
        std::filesystem::create_directory(linked / dead);
        std::ofstream(linked / dead / "mcp.json") << "{}";
        const auto       own = std::filesystem::weakly_canonical(linked / dead);
        discovery::Entry entry{.session = dead, .project = "/work/project", .endpoint = own.generic_string(), .allow = "[]", .pid = 2147483000L,
                               .mcp = "http://127.0.0.1:1/mcp", .configuration = (own / "mcp.json").generic_string()};
        channel::writeEndpoint(linked / (dead + ".json"), entry.json());
        then("listing through the alias still removes its private directory", [&] {
            require(discovery::list(linked).empty());
            require(!std::filesystem::exists(real / "sessions" / dead));
        });
        then("a session directory replaced by a link is left alone", [&] {
            const auto elsewhere = root.path() / "elsewhere";
            std::filesystem::create_directory(elsewhere);
            std::ofstream(elsewhere / "mcp.json") << "{}";
            std::filesystem::remove_all(linked / dead);
            std::filesystem::create_directory_symlink(elsewhere, linked / dead);
            require(discovery::list(linked).empty());
            require(std::filesystem::exists(elsewhere / "mcp.json"));
            std::filesystem::remove(linked / dead);
        });
    });
#endif
    given("paths so long that the record would exceed its limit", [&] {
        then("the record is refused before anything is published", [&] {
            const auto       session = std::string(64, 'f');
            const auto       far     = "/" + std::string(discovery::recordLimit / 2, 'p');
            discovery::Entry entry{.session = session, .project = far, .endpoint = far, .allow = "[]", .pid = discovery::currentProcess(),
                                   .mcp = "http://127.0.0.1:1/mcp", .configuration = far + "/mcp.json"};
            require(entry.json().size() > discovery::recordLimit);
            requireThrows([&] { discovery::Registration refused{sessions, entry}; });
            require(!std::filesystem::exists(sessions / (session + ".json")));
        });
    });
}};
