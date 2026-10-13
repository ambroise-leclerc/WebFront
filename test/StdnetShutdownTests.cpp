#include <http/HTTPServer.hpp>
#include <networking/TCPNetworkingStdnet.hpp>
#include <WebFront.hpp>
#include <system/DefaultFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
#include <array>
#include <chrono>
#include <string>
#include <stdexcept>
#include <thread>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

using namespace webfront;
using namespace std::chrono_literals;

namespace {

// Counts the live sockets handed to connections: a leaked connection keeps its socket alive.
struct CountingNet : networking::TCPStdnet {
    static inline std::atomic<int> liveSockets{0};

    // Writes on sockets accepted after armHold() are held back instead of being started, so a test can
    // decide when an HTTP upgrade completes. They are released by IoContext::poll(), which Server::drain() runs.
    using HeldWrite = std::function<void(std::error_code, std::size_t)>;
    static inline std::mutex heldMutex;
    static inline std::vector<HeldWrite> heldWrites;
    static inline bool holdNewSockets = false; // sockets accepted while set have their writes held
    // Reads started on sockets, and their number when the held upgrade completions were released:
    // a WebSocket created by a late upgrade starts reading immediately.
    static inline std::atomic<int> reads{0};
    static inline std::atomic<int> readsAtRelease{-1};

    static std::size_t heldCount() {
        std::lock_guard lock(heldMutex);
        return heldWrites.size();
    }
    static void armHold() {
        std::lock_guard lock(heldMutex);
        holdNewSockets = true;
    }

    template<class SocketType, class Buffers, class Handler>
    static void AsyncWrite(SocketType& socket, Buffers&& buffers, Handler&& handler) {
        if (socket.holdsWrites) {
            std::lock_guard lock(heldMutex);
            heldWrites.emplace_back(std::forward<Handler>(handler));
            return;
        }
        TCPStdnet::AsyncWrite(socket, std::forward<Buffers>(buffers), std::forward<Handler>(handler));
    }

    class IoContext : public TCPStdnet::IoContext {
    public:
        std::size_t poll() {
            std::vector<HeldWrite> released;
            {
                std::lock_guard lock(heldMutex);
                released.swap(heldWrites);
            }
            if (!released.empty()) readsAtRelease = reads.load();
            for (auto& write : released) write(std::error_code{}, 0);
            return TCPStdnet::IoContext::poll() + released.size();
        }
    };

    class Socket : public TCPStdnet::Socket {
    public:
        bool holdsWrites = false;

        explicit Socket(IoContext& ctx) noexcept : TCPStdnet::Socket(ctx) { ++liveSockets; }
        Socket(TCPStdnet::Socket&& s) noexcept : TCPStdnet::Socket(std::move(s)) {
            ++liveSockets;
            std::lock_guard lock(heldMutex);
            holdsWrites = holdNewSockets;
        }
        Socket(Socket&& other) noexcept : TCPStdnet::Socket(std::move(other)), holdsWrites(other.holdsWrites) { ++liveSockets; }
        Socket& operator=(Socket&&) noexcept = default;
        ~Socket() { --liveSockets; }

        template<class Buffers, class Handler>
        void async_read_some(const Buffers& buffers, Handler&& handler) {
            ++reads;
            TCPStdnet::Socket::async_read_some(buffers, std::forward<Handler>(handler));
        }
    };

    class Acceptor : public TCPStdnet::Acceptor {
    public:
        explicit Acceptor(IoContext& ctx) noexcept : TCPStdnet::Acceptor(ctx) {}

        template<class Handler>
        void async_accept(Handler&& handler) {
            TCPStdnet::Acceptor::async_accept([h = std::forward<Handler>(handler)](std::error_code ec, TCPStdnet::Socket&& s) mutable {
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
    GIVEN("A WebFront with an established WebSocket and a second upgrade whose completion is held back") {
        const auto port = freePort();
        CountingNet::readsAtRelease = -1;
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
            // Hold the second upgrade's response write: its completion (which hands the socket to a new
            // WebLink) is released only by the drain that runs while WebFront is being destroyed.
            CountingNet::armHold();
            second.connect(endpoint);
            second.send(std::as_bytes(std::span(request)));
            for (int i = 0; i < 500 && CountingNet::heldCount() == 0; ++i) std::this_thread::sleep_for(10ms);
            CHECK(CountingNet::heldCount() > 0);
            webFront.stop();
            serverThread.join();
        }
        THEN("no socket is leaked and the late upgrade creates no WebLink") {
            REQUIRE(CountingNet::liveSockets.load() == 0);
            REQUIRE(CountingNet::readsAtRelease.load() >= 0);
            REQUIRE(CountingNet::reads.load() == CountingNet::readsAtRelease.load());
        }
    }
}

SCENARIO("Destroying the stdnet provider's context releases abandoned operations without invoking them") {
    GIVEN("An accept and a read pending on objects owned by their own handlers") {
        const auto port = freePort();
        std::weak_ptr<int> acceptCapture;
        std::weak_ptr<int> readCapture;
        bool invoked = false;
        {
            networking::TCPStdnet::IoContext context;
            networking::TCPStdnet::Resolver resolver(context);
            auto endpoint = *resolver.resolve("127.0.0.1", port).begin();
            networking::TCPStdnet::Acceptor acceptor(context);
            acceptor.open(endpoint.protocol());
            acceptor.bind(endpoint);
            acceptor.listen();

            auto onAccept = std::make_shared<int>(1);
            acceptCapture = onAccept;
            acceptor.async_accept([onAccept, &invoked](std::error_code, networking::TCPStdnet::Socket) { invoked = true; });

            // A connection whose read handler owns the socket it reads from, as WebFront's connections do.
            networking::TCPStdnet::IoContext clientContext;
            networking::TCPStdnet::Socket client(clientContext);
            client.connect(endpoint);
            auto owner = std::make_shared<std::pair<int, std::unique_ptr<networking::TCPStdnet::Socket>>>(
                2, std::make_unique<networking::TCPStdnet::Socket>(context));
            owner->second->connect(endpoint);
            readCapture = std::shared_ptr<int>(owner, &owner->first);
            std::array<char, 8> buffer{};
            auto* socket = owner->second.get();
            socket->async_read_some(networking::TCPStdnet::Buffer(buffer),
                                    [owner = std::move(owner), &invoked](std::error_code, std::size_t) { invoked = true; });
            // The context is destroyed here without being run again.
        }
        THEN("their handlers are destroyed but never invoked") {
            REQUIRE(acceptCapture.expired());
            REQUIRE(readCapture.expired());
            REQUIRE_FALSE(invoked);
        }
    }
}
