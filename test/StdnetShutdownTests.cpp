#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <http/HTTPServer.hpp>
#include <networking/TCPNetworkingStdnet.hpp>
#include <system/DefaultFS.hpp>
#include <WebFront.hpp>

#ifdef _WIN32
    #include <system/WindowsCompat.hpp>
#else
    #include <poll.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

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

void waitUntil(auto condition) {
    for (int i = 0; i < 500 && !condition(); ++i) std::this_thread::sleep_for(10ms);
}

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

const std::string upgradeRequest =
    "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

}  // namespace

SCENARIO("The HTTP listener is ready before its event loop starts") {
    const auto port = freePort();
    http::Server<Stdnet, fs::DefaultFS> server("127.0.0.1", port);
    Stdnet::IoContext clientContext;
    Stdnet::Socket client(clientContext);
    // No server thread and no run()/runOne(): a synchronous connection must already succeed.
    REQUIRE_NOTHROW(connect(client, port));
    REQUIRE(client.is_open());
}

SCENARIO("Stopping a running server releases its connections") {
    GIVEN("A running server with an idle client connection") {
        const auto port = freePort();
        Stdnet::IoContext clientContext;
        Stdnet::Socket    client(clientContext);
        {
            http::Server<CountingNet, fs::DefaultFS> server("127.0.0.1", port);
            std::thread                              serverThread([&server] { server.run(); });
            connect(client, port);
            waitUntil([] { return CountingNet::liveSockets.load() > 0; });
            CHECK(CountingNet::liveSockets.load() > 0);

            server.stop();  // from another thread than the one running the server
            serverThread.join();
        }
        THEN("no socket is left once the server is destroyed") { REQUIRE(CountingNet::liveSockets.load() == 0); }
    }
}

SCENARIO("Destroying a server which is not running releases its connections") {
    GIVEN("A server which accepted a client connection") {
        const auto port = freePort();
        Stdnet::IoContext clientContext;
        Stdnet::Socket    client(clientContext);
        {
            http::Server<CountingNet, fs::DefaultFS> server("127.0.0.1", port);
            connect(client, port);
            for (int i = 0; i < 10 && CountingNet::liveSockets.load() == 0; ++i) server.runOne();
            CHECK(CountingNet::liveSockets.load() > 0);
        }
        THEN("no socket is left") { REQUIRE(CountingNet::liveSockets.load() == 0); }
    }
}

SCENARIO("Destroying WebFront closes its established WebSockets") {
    GIVEN("A WebFront with an established WebSocket") {
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
            CHECK(readHeaders(client).starts_with("HTTP/1.1 101"));
            // The HTTP response can arrive before onUpgrade creates the WebLink. Complete the
            // masked bridge handshake before stopping, so this tests an established connection.
            const std::array<unsigned char, 8>
                handshake{0x82, 0x82, 0, 0, 0, 0, static_cast<unsigned char>(msg::Command::handshake), static_cast<unsigned char>(msg::JSEndian::little)};
            client.send(Stdnet::ConstBuffer(handshake.data(), handshake.size()));
            waitUntil([&] {
                return linked.load();
            });
            CHECK(linked.load());
            webFront.stop();
            serverThread.join();
        }
        THEN("no socket is left") {
            REQUIRE(CountingNet::liveSockets.load() == 0);
        }
    }
}

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

SCENARIO("Application task cancellation returns to the server context") {
    GIVEN("an application task running on a real stdnet context") {
        TaskThreadState                state;
        BasicWF<Stdnet, fs::DefaultFS> app(WebFrontConfig{.port = "0"});
        auto                           handle = app.spawn(applicationTaskOnServer(state));
        std::thread                    serverThread([&] {
            app.run();
        });
        const auto                     serverId = serverThread.get_id();
        waitUntil([&] {
            return state.started.load(std::memory_order_acquire);
        });
        CHECK(state.started.load(std::memory_order_acquire));
        WHEN("another thread requests cancellation") {
            handle.requestStop();
            waitUntil([&] {
                return state.destroyed.load(std::memory_order_acquire);
            });
            app.stop();
            serverThread.join();
            app.shutdown();
            THEN("the coroutine starts and unwinds on the server context thread") {
                CHECK(state.destroyed.load(std::memory_order_acquire));
                CHECK(state.startedOn == serverId);
                CHECK(state.destroyedOn == serverId);
            }
        }
    }
}

SCENARIO("A rejected WebSocket Origin never reaches the server upgrade callback") {
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
    THEN("a foreign origin gets 403 without reaching the upgrade callback") {
        CHECK(response.starts_with("HTTP/1.1 403 Forbidden\r\n"));
        CHECK(response.find("Sec-WebSocket-Accept:") == std::string::npos);
        REQUIRE(upgrades == 0);
    }
    THEN("the same listener accepts the configured origin and invokes that callback") {
        Stdnet::Socket authorized(context);
        connect(authorized, port);
        const auto allowedRequest = upgradeRequest.substr(0, upgradeRequest.size() - 2) + "Origin: http://127.0.0.1:" + port + "\r\n\r\n";
        authorized.send(std::as_bytes(std::span(allowedRequest)));
        CHECK(readHeaders(authorized).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
        waitUntil([&] {
            return upgrades.load() != 0;
        });
        REQUIRE(upgrades == 1);
    }
    THEN("an idle peer cannot block the header reader indefinitely") {
        Stdnet::Socket idle(context);
        connect(idle, port);
        REQUIRE_THROWS_WITH(readHeaders(idle, 0ms), "Timed out waiting for HTTP response headers");
        REQUIRE_THROWS_WITH(readHeaders(idle, 10ms), "Timed out waiting for HTTP response headers");
    }
    serverThread.request_stop();
    serverThread.join();
}
