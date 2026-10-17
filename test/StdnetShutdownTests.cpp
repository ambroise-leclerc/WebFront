#include "InjectableNetworking.hpp"
#include <WebFront.hpp>
#include <http/HTTPServer.hpp>
#include <networking/TCPNetworkingStdnet.hpp>
#include <system/DefaultFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <span>
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

// Reads the HTTP response head of a blocking client socket.
std::string readHeaders(Stdnet::Socket& socket) {
    std::string           received;
    std::array<char, 512> buffer{};
    while (received.find("\r\n\r\n") == std::string::npos) {
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
