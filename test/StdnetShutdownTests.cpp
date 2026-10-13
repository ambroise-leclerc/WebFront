#include <http/HTTPServer.hpp>
#include <networking/TCPNetworkingStdnet.hpp>
#include <WebFront.hpp>
#include <system/DefaultFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <array>
#include <chrono>
#include <string>
#include <stdexcept>
#include <thread>

#include <atomic>

using namespace webfront;
using namespace std::chrono_literals;

namespace {

// Counts the live sockets handed to connections: a leaked connection keeps its socket alive.
struct CountingNet : networking::TCPStdnet {
    static inline std::atomic<int> liveSockets{0};
    static inline std::atomic<int> acceptedSockets{0};

    class Socket : public TCPStdnet::Socket {
    public:
        explicit Socket(IoContext& ctx) noexcept : TCPStdnet::Socket(ctx) { ++liveSockets; }
        Socket(TCPStdnet::Socket&& s) noexcept : TCPStdnet::Socket(std::move(s)) { ++liveSockets; }
        Socket(Socket&& other) noexcept : TCPStdnet::Socket(std::move(other)) { ++liveSockets; }
        Socket& operator=(Socket&&) noexcept = default;
        ~Socket() { --liveSockets; }
    };

    class Acceptor : public TCPStdnet::Acceptor {
    public:
        explicit Acceptor(IoContext& ctx) noexcept : TCPStdnet::Acceptor(ctx) {}

        template<class Handler>
        void async_accept(Handler&& handler) {
            TCPStdnet::Acceptor::async_accept([h = std::forward<Handler>(handler)](std::error_code ec, TCPStdnet::Socket&& s) mutable {
                if (!ec) ++acceptedSockets;
                h(ec, Socket(std::move(s)));
            });
        }
    };
};

} // namespace

namespace {

std::string freePort() {
    networking::TCPStdnet::IoContext ctx;
    networking::TCPStdnet::Acceptor acceptor(ctx);
    networking::TCPStdnet::Resolver resolver(ctx);
    auto endpoint = *resolver.resolve("127.0.0.1", "0").begin();
    acceptor.open(endpoint.protocol());
    acceptor.bind(endpoint);
    return std::to_string(acceptor.local_endpoint().port());
}

} // namespace

SCENARIO("Server shutdown releases pending asynchronous operations") {
    GIVEN("A server with an idle client connection") {
        const auto port = freePort();
        networking::TCPStdnet::IoContext clientContext;
        networking::TCPStdnet::Socket client(clientContext);
        {
            http::Server<CountingNet, fs::DefaultFS> server("127.0.0.1", port);
            networking::TCPStdnet::Resolver resolver(clientContext);
            client.connect(*resolver.resolve("127.0.0.1", port).begin());
            // Accept the client, then stop without letting the context run the cancellation completions.
            for (int i = 0; i < 10 && CountingNet::liveSockets.load() == 0; ++i) server.runOne();
            REQUIRE(CountingNet::liveSockets.load() > 0);

            server.stop();
        }
        THEN("no connection or socket is leaked once the server is destroyed") { REQUIRE(CountingNet::liveSockets.load() == 0); }
    }
}

SCENARIO("A throwing completion handler is contained by the stdnet provider") {
    GIVEN("A pending accept whose handler throws") {
        const auto port = freePort();
        networking::TCPStdnet::IoContext context;
        networking::TCPStdnet::Acceptor acceptor(context);
        networking::TCPStdnet::Resolver resolver(context);
        auto endpoint = *resolver.resolve("127.0.0.1", port).begin();
        acceptor.open(endpoint.protocol());
        acceptor.bind(endpoint);
        acceptor.listen();
        acceptor.async_accept([](std::error_code, networking::TCPStdnet::Socket) { throw std::runtime_error("handler failure"); });

        WHEN("a client connects and the context runs") {
            networking::TCPStdnet::Socket client(context);
            client.connect(endpoint);
            THEN("the exception does not escape the context") { REQUIRE_NOTHROW(context.run_one()); }
        }
    }
}

namespace {

using Clock = std::chrono::steady_clock;

// Performs one read on `socket`, appending to `received`. Returns false on error or when `deadline` passes.
bool readSome(networking::TCPStdnet::IoContext& context, networking::TCPStdnet::Socket& socket, std::string& received,
              Clock::time_point deadline) {
    std::array<char, 512> buffer{};
    bool done = false;
    std::error_code failure;
    socket.async_read_some(networking::TCPStdnet::Buffer(buffer), [&](std::error_code ec, std::size_t size) {
        failure = ec;
        if (!ec) received.append(buffer.data(), size);
        done = true;
    });
    while (!done && Clock::now() < deadline) context.run_for(100ms);
    if (!done) {
        // Timed out: the pending read references these locals. Cancel it and run its completion before they go.
        std::error_code ignored;
        socket.close(ignored);
        while (!done) context.run_for(100ms);
        return false;
    }
    return !failure;
}

// Reads from `socket` until the end of the HTTP headers, failing after five seconds.
std::string readHeaders(networking::TCPStdnet::IoContext& context, networking::TCPStdnet::Socket& socket) {
    std::string received;
    const auto deadline = Clock::now() + 5s;
    while (received.find("\r\n\r\n") == std::string::npos && readSome(context, socket, received, deadline)) {}
    return received;
}

} // namespace

SCENARIO("Destroying WebFront closes established WebSockets and completes pending upgrades") {
    GIVEN("A WebFront with an established WebSocket and a second upgrade request in flight") {
        const auto port = freePort();
        CountingNet::acceptedSockets = 0;
        networking::TCPStdnet::IoContext clientContext;
        networking::TCPStdnet::Resolver resolver(clientContext);
        const auto endpoint = *resolver.resolve("127.0.0.1", port).begin();
        const std::string request =
            "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        networking::TCPStdnet::Socket first(clientContext);
        networking::TCPStdnet::Socket second(clientContext);
        {
            BasicWF<CountingNet, fs::DefaultFS> webFront(WebFrontConfig{.address = "127.0.0.1", .port = port});
            std::thread serverThread([&webFront] { webFront.run(); });
            first.connect(endpoint);
            first.send(std::as_bytes(std::span(request)));
            // The 101 response proves the first WebSocket is established before anything is shut down.
            // CHECK, not REQUIRE: the server thread must still be stopped and joined on failure.
            CHECK(readHeaders(clientContext, first).starts_with("HTTP/1.1 101"));
            second.connect(endpoint);
            second.send(std::as_bytes(std::span(request)));
            // Wait until the server has accepted the second connection; its upgrade may still be in flight
            // at stop() (that interleaving is timing dependent and is also covered by the sanitizer jobs).
            for (int i = 0; i < 500 && CountingNet::acceptedSockets.load() < 2; ++i) std::this_thread::sleep_for(10ms);
            CHECK(CountingNet::acceptedSockets.load() >= 2);
            webFront.stop();
            serverThread.join();
        }
        THEN("no socket is leaked and no handler touches destroyed state") { REQUIRE(CountingNet::liveSockets.load() == 0); }
    }
}
