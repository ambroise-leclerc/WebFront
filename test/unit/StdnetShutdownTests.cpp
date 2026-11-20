#include "InjectableNetworking.hpp"
#include "Bdd.hpp"
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/WebFront.hpp>
#include "../../examples/PortOption.hpp"

#ifdef _WIN32
    #include <webfront/system/WindowsCompat.hpp>
#else
    #include <poll.h>
#endif

#include <array>
#include <cstdlib>
#include <atomic>
#include <future>
#include <chrono>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <memory>
#include <set>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;
using namespace std::chrono_literals;
using Stdnet = networking::TCPStdnet;
namespace ex = networking::ex;

namespace {

// Counts the sockets handed to the server: a connection which is not released keeps its socket alive.
struct CountingNet : Stdnet {
    static inline std::atomic<int> liveSockets{0};

    struct Socket : Stdnet::Socket {
        Socket(Stdnet::Socket&& accepted) noexcept : Stdnet::Socket(std::move(accepted)) { ++liveSockets; }
        Socket(Socket&& other) noexcept : Stdnet::Socket(std::move(other)) { ++liveSockets; }
        ~Socket() { --liveSockets; }
    };

    static auto Accept(Acceptor& acceptor) {
        return ex::then(Stdnet::Accept(acceptor), [](Stdnet::Socket accepted) { return Socket(std::move(accepted)); });
    }
};

std::string freePort() {
    Stdnet::IoContext context;
    Stdnet::Acceptor  acceptor(context);
    const auto        endpoint = Stdnet::Resolve("127.0.0.1", "0");
    acceptor.open(endpoint.protocol());
    acceptor.bind(endpoint);
    return std::to_string(acceptor.local_endpoint().port());
}

void connect(Stdnet::Socket& client, const std::string& port) { client.connect(Stdnet::Resolve("127.0.0.1", port)); }

void waitForHeaderData(Stdnet::Socket& socket, std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0)
        throw std::runtime_error("Timed out waiting for HTTP response headers");
    ::pollfd descriptor{};
    descriptor.fd     = socket.native_handle();
    descriptor.events = POLLIN;
#ifdef _WIN32
    const int ready = ::WSAPoll(&descriptor, 1, static_cast<int>(remaining));
#else
    const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
#endif
    if (ready < 0)
        throw std::runtime_error("HTTP response readiness wait failed");
    if (ready == 0)
        throw std::runtime_error("Timed out waiting for HTTP response headers");
}

// A single deadline bounds the whole response head, even if bytes arrive in small chunks.
std::string readHeaders(Stdnet::Socket& socket, std::chrono::milliseconds timeout = 5s) {
    const auto            deadline = std::chrono::steady_clock::now() + timeout;
    std::string           received;
    std::array<char, 512> buffer{};
    while (received.find("\r\n\r\n") == std::string::npos) {
        waitForHeaderData(socket, deadline);
        const auto size = socket.receive(Stdnet::Buffer(buffer));
        if (size == 0) break;
        received.append(buffer.data(), size);
    }
    return received;
}

webfront::Task cleanupReleasesHandler(http::Server<Stdnet, fs::DefaultFS>& server, std::shared_ptr<void> guard) {
    static_cast<void>(guard);  // released when the cancelled task is destroyed, on the server context
    co_await server.resumeAfter(1h);
}

const std::string upgradeRequest =
    "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

}  // namespace

const Scenario hTTPListener{"The HTTP listener is ready before its event loop starts", [] {
    const auto port = freePort();
    http::Server<Stdnet, fs::DefaultFS> server("127.0.0.1", port);
    Stdnet::IoContext clientContext;
    Stdnet::Socket client(clientContext);
    // No server thread and no run()/runOne(): a synchronous connection must already succeed.
    requireNothrow([&] { connect(client, port); });
    require(client.is_open());
}};

const Scenario acceptedConnections{"Accepted connections retain TCP_NODELAY through WebSocket upgrade", [] {
    given("A real stdnet HTTP server and a loopback client", [&] {
        const auto                          port = freePort();
        http::Server<Stdnet, fs::DefaultFS> server("127.0.0.1", port);
        Stdnet::IoContext                   clientContext;
        Stdnet::Socket                      client(clientContext);
        bool                                upgraded = false;
        bool                                noDelay  = false;
        server.onUpgrade([&](Stdnet::Socket&& socket) {
            ::stdnet::ip::tcp_no_delay option;
            socket.get_option(option);
            noDelay  = option.value();
            upgraded = true;
            server.stop();
        });
        connect(client, port);
        client.send(std::as_bytes(std::span(upgradeRequest)));
        server.run();
        then("the socket passed to the bridge has Nagle disabled", [&] {
            require(upgraded);
            require(noDelay);
        });
    });
}};

const Scenario stoppingRunning{"Stopping a running server releases its connections", [] {
    given("A running server with an idle client connection", [&] {
        const auto port = freePort();
        Stdnet::IoContext clientContext;
        Stdnet::Socket    client(clientContext);
        {
            http::Server<CountingNet, fs::DefaultFS> server("127.0.0.1", port);
            std::thread                              serverThread([&server] { server.run(); });
            connect(client, port);
            checkEventually([] { return CountingNet::liveSockets.load() > 0; }, 5s);

            server.stop();  // from another thread than the one running the server
            serverThread.join();
        }
        then("no socket is left once the server is destroyed", [&] { require(CountingNet::liveSockets.load() == 0); });
    });
}};

const Scenario destroyingServer{"Destroying a server which is not running releases its connections", [] {
    given("A server which accepted a client connection", [&] {
        const auto port = freePort();
        Stdnet::IoContext clientContext;
        Stdnet::Socket    client(clientContext);
        {
            http::Server<CountingNet, fs::DefaultFS> server("127.0.0.1", port);
            connect(client, port);
            for (int i = 0; i < 10 && CountingNet::liveSockets.load() == 0; ++i) server.runOne();
            check(CountingNet::liveSockets.load() > 0);
        }
        then("no socket is left", [&] { require(CountingNet::liveSockets.load() == 0); });
    });
}};

const Scenario destroyingWebFront{"Destroying WebFront closes its established WebSockets", [] {
    given("A WebFront with an established WebSocket", [&] {
        const auto        port = freePort();
        Stdnet::IoContext clientContext;
        Stdnet::Socket    client(clientContext);
        {
            BasicWF<CountingNet, fs::DefaultFS> webFront(WebFrontConfig{.address = "127.0.0.1", .port = port});
            std::atomic<bool>                   linked = false;
            webFront.onUIStarted([&](auto) {
                linked = true;
            });
            std::thread serverThread(&decltype(webFront)::run, &webFront);
            connect(client, port);
            client.send(std::as_bytes(std::span(upgradeRequest)));
            check(readHeaders(client).starts_with("HTTP/1.1 101"));
            // The HTTP response can arrive before onUpgrade creates the WebLink. Complete the
            // masked bridge handshake before stopping, so this tests an established connection.
            const std::array<unsigned char, 8>
                handshake{0x82, 0x82, 0, 0, 0, 0, static_cast<unsigned char>(msg::Command::handshake), static_cast<unsigned char>(msg::JSEndian::little)};
            client.send(Stdnet::ConstBuffer(handshake.data(), handshake.size()));
            checkEventually(
                [&] {
                    return linked.load();
                },
                5s);
            webFront.stop();
            serverThread.join();
        }
        then("no socket is left", [&] {
            require(CountingNet::liveSockets.load() == 0);
        });
    });
}};

namespace {
struct TaskThreadState {
    std::thread::id   startedOn;
    std::thread::id   destroyedOn;
    std::atomic<bool> started{false};
    std::atomic<bool> destroyed{false};
};
struct TaskThreadGuard {
    TaskThreadState& state;
    ~TaskThreadGuard() {
        state.destroyedOn = std::this_thread::get_id();
        state.destroyed.store(true, std::memory_order_release);
    }
};
Task applicationTaskOnServer(TaskThreadState& state) {
    TaskThreadGuard guard{state};
    state.startedOn = std::this_thread::get_id();
    co_await testing::ManualSender([&state](auto) {
        state.started.store(true, std::memory_order_release);
    });
}
}  // namespace

const Scenario applicationTask{"Application task cancellation returns to the server context", [] {
    given("an application task running on a real stdnet context", [&] {
        TaskThreadState                state;
        BasicWF<Stdnet, fs::DefaultFS> app(WebFrontConfig{.port = "0"});
        auto                           handle = app.spawn(applicationTaskOnServer(state));
        std::thread                    serverThread([&] {
            app.run();
        });
        const auto                     serverId = serverThread.get_id();
        checkEventually(
            [&] {
                return state.started.load(std::memory_order_acquire);
            },
            5s);
        when("another thread requests cancellation", [&] {
            handle.requestStop();
            checkEventually(
                [&] {
                    return state.destroyed.load(std::memory_order_acquire);
                },
                5s);
            app.stop();
            serverThread.join();
            app.shutdown();
            then("the coroutine starts and unwinds on the server context thread", [&] {
                check(state.destroyed.load(std::memory_order_acquire));
                check(state.startedOn == serverId);
                check(state.destroyedOn == serverId);
            });
        });
    });
}};

const Scenario rejectedWebSocket{"A rejected WebSocket Origin never reaches the server upgrade callback", [] {
    const auto                          port = freePort();
    http::Server<Stdnet, fs::DefaultFS> server("127.0.0.1", port);
    std::atomic<int>                    upgrades = 0;
    server.onUpgrade([&](Stdnet::Socket&&) {
        ++upgrades;
    });
    std::jthread      serverThread([&](std::stop_token token) {
        std::stop_callback stop(token, [&] {
            server.stop();
        });
        server.run();
    });
    Stdnet::IoContext context;
    Stdnet::Socket    client(context);
    connect(client, port);
    const auto request = upgradeRequest.substr(0, upgradeRequest.size() - 2) + "Origin: https://untrusted.example\r\n\r\n";
    client.send(std::as_bytes(std::span(request)));
    const auto response = readHeaders(client);
    then("a foreign origin gets 403 without reaching the upgrade callback", [&] {
        check(response.starts_with("HTTP/1.1 403 Forbidden\r\n"));
        check(response.find("Sec-WebSocket-Accept:") == std::string::npos);
        require(upgrades == 0);
    });
    then("the same listener accepts the configured origin and invokes that callback", [&] {
        Stdnet::Socket authorized(context);
        connect(authorized, port);
        const auto allowedRequest = upgradeRequest.substr(0, upgradeRequest.size() - 2) + "Origin: http://127.0.0.1:" + port + "\r\n\r\n";
        authorized.send(std::as_bytes(std::span(allowedRequest)));
        check(readHeaders(authorized).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
        checkEventually(
            [&] {
                return upgrades.load() != 0;
            },
            5s);
        require(upgrades == 1);
    });
    then("an idle peer cannot block the header reader indefinitely", [&] {
        Stdnet::Socket idle(context);
        connect(idle, port);
        requireThrows([&] { readHeaders(idle, 0ms); }, equals("Timed out waiting for HTTP response headers"));
        requireThrows([&] { readHeaders(idle, 10ms); }, equals("Timed out waiting for HTTP response headers"));
    });
    serverThread.request_stop();
    serverThread.join();
}};

const Scenario drainingPumps{"Draining pumps cancelled tasks while a route handler waits for their cleanup", [] {
    const auto                          port = freePort();
    http::Server<Stdnet, fs::DefaultFS> server("127.0.0.1", port);
    std::promise<void>                  released, entered;
    std::atomic<bool>                   done{false};
    const auto                          release = [&] {
        if (!done.exchange(true)) released.set_value();
    };
    auto route = server.addRoute("/webfront/services/wait/", [&, wait = released.get_future().share()](const http::RouteRequest&) {
        entered.set_value();
        wait.wait();
        return http::RouteReply{};
    });
    {
        std::shared_ptr<void> guard(nullptr, [&](void*) { release(); });
        static_cast<void>(server.startTask(cleanupReleasesHandler(server, std::move(guard))));
    }
    std::thread       serverThread([&server] { server.run(); });
    Stdnet::IoContext clientContext;
    Stdnet::Socket    client(clientContext);
    connect(client, port);
    const std::string request = "GET /webfront/services/wait/x HTTP/1.1\r\nHost: localhost\r\n\r\n";
    client.send(std::as_bytes(std::span(request)));
    auto       enteredFuture  = entered.get_future();
    const bool handlerEntered = waitReady(enteredFuture, 5s);
    server.stop();
    serverThread.join();  // before any assertion: a joinable thread would terminate the process
    require(handlerEntered);
    when("the server is drained", [&] {
        auto drained = std::async(std::launch::async, [&server] { server.drain(); });
        const bool finished = waitReady(drained, 5s);
        release();  // never leave the test hanging if the drain waited for the cleanup it was to run
        drained.wait();
        then("the drain ran the cleanup which released the handler", [&] { require(finished); });
    });
}};

const Scenario simultaneousInstances{"Servers side by side on ephemeral ports", [] {
    given("a server that holds a port", [&] {
        http::Server<Stdnet, fs::DefaultFS> first("127.0.0.1", "0");
        then("a second server on port \"0\" gets another port", [&] {
            http::Server<Stdnet, fs::DefaultFS> second("127.0.0.1", "0");
            require(first.port() != second.port());
            require(second.port() != "0");
        });
        then("a server asking for the same port reports the address, the port and the cause", [&] {
            const std::string port{first.port()};
            requireThrows<std::system_error>([&] { http::Server<Stdnet, fs::DefaultFS> clash("127.0.0.1", port); },
                                             contains("127.0.0.1:" + port));
            requireThrows<std::system_error>([&] { http::Server<Stdnet, fs::DefaultFS> clash("127.0.0.1", port); }, contains("WEBFRONT_PORT=0"));
        });
    });
    given("many servers without a configured port", [&] {
        std::vector<std::unique_ptr<http::Server<Stdnet, fs::DefaultFS>>> servers;
        for (int i = 0; i < 40; ++i)
            servers.push_back(std::make_unique<http::Server<Stdnet, fs::DefaultFS>>("127.0.0.1", "0"));
        then("each one listens on its own port", [&] {
            std::set<std::string> ports;
            for (const auto& server : servers)
                ports.insert(std::string(server->port()));
            require(ports.size() == servers.size());
        });
    });
}};

void setPortVariable(const char* value) {
#ifdef _WIN32
    _putenv_s("WEBFRONT_PORT", value);  // an empty value removes the variable on Windows, which defaultPort() treats alike
#else
    setenv("WEBFRONT_PORT", value, 1);
#endif
}

const Scenario defaultPortFromEnvironment{"The default port follows WEBFRONT_PORT", [] {
    given("WEBFRONT_PORT set to 0", [&] {
        setPortVariable("0");
        then("a configuration asks for an ephemeral port and two applications coexist", [&] {
            require(WebFrontConfig{}.port == "0");
            BasicWF<Stdnet, fs::DefaultFS> a{WebFrontConfig{}}, b{WebFrontConfig{}};
            require(a.port() != b.port());
        });
        setPortVariable("");
    });
    given("WEBFRONT_PORT unset or empty", [&] {
        setPortVariable("");
        then("the historical 9002 remains", [&] { require(defaultPort() == "9002"); });
        setPortVariable("");
    });
}};

const Scenario portValidation{"A port is a number from 0 to 65535", [] {
    given("values the resolver would accept", [&] {
        then("only canonical port numbers pass", [&] {
            require(validatedPort("0", "t") == "0");
            require(validatedPort("9002", "t") == "9002");
            require(validatedPort("65535", "t") == "65535");
            for (const char* bad : {"", "00", "65536", "http", "-1", "+80", "80 ", "080", "123456"})
                requireThrows<std::invalid_argument>([&] { (void)validatedPort(bad, "WEBFRONT_PORT"); }, contains("WEBFRONT_PORT"));
        });
        then("an invalid WEBFRONT_PORT makes the default configuration fail", [&] {
            setPortVariable("http");
            requireThrows<std::invalid_argument>([&] { (void)defaultPort(); }, contains("'http'"));
            setPortVariable("");
        });
    });
    given("a server asked for port 00", [&] {
        http::Server<Stdnet, fs::DefaultFS> server("127.0.0.1", "00");
        then("it announces the port really bound", [&] { require(server.port() != "00" && server.port() != "0"); });
    });
}};

const Scenario portOption{"The examples take --port", [] {
    given("--port <n> and --port=<n> among other arguments", [&] {
        setPortVariable("");
        char  program[] = "app", flag[] = "--port", value[] = "0", other[] = "project", joined[] = "--port=0";
        char* argv[]    = {program, other, flag, value, joined, nullptr};
        int   argc      = 5;
        then("they set the default port and leave the other arguments", [&] {
            require(demo::applyPortOption(argc, argv));
            require(argc == 2 && std::string(argv[1]) == "project" && argv[2] == nullptr);
            require(defaultPort() == "0");
            setPortVariable("");
        });
    });
    given("an invalid or missing value", [&] {
        char  program[] = "app", flag[] = "--port", bad[] = "65536";
        char* withBad[] = {program, flag, bad, nullptr};
        char* missing[] = {program, flag, nullptr};
        int   badCount = 3, missingCount = 2;
        then("the option is refused", [&] {
            require(!demo::applyPortOption(badCount, withBad));
            require(!demo::applyPortOption(missingCount, missing));
        });
    });
}};
