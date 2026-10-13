#include <http/HTTPServer.hpp>
#include <networking/TCPNetworkingStdnet.hpp>
#include <system/DefaultFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace webfront;

namespace {

// Counts the live sockets handed to connections: a leaked connection keeps its socket alive.
struct CountingNet : networking::TCPStdnet {
    static inline std::atomic<int> liveSockets{0};

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
