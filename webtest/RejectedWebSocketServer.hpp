#pragma once

#include <networking/TCPNetworkingTS.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

// A test-owned endpoint that accepts HTTP requests but always refuses WebSocket upgrades.
// Binding port zero keeps the initial-connection-failure tests independent of other services.
class RejectedWebSocketServer {
    using Net = webfront::networking::TCPNetworkingTS;

    struct Session : std::enable_shared_from_this<Session> {
        Net::Socket                       socket;
        std::atomic<unsigned>&            rejected;
        std::array<char, 1024>            buffer{};
        std::string                       request;
        static constexpr std::string_view response{"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};

        Session(Net::Socket connection, std::atomic<unsigned>& count) : socket(std::move(connection)), rejected(count) { }

        void read() {
            socket.async_read_some(Net::Buffer(buffer), [self = shared_from_this()](std::error_code error, std::size_t size) {
                if (error)
                    return;
                self->request.append(self->buffer.data(), size);
                if (self->request.find("\r\n\r\n") != std::string::npos) {
                    ++self->rejected;
                    Net::AsyncWrite(self->socket, Net::Buffer(response), [self](std::error_code, std::size_t) {
                        std::error_code ignored;
                        self->socket.close(ignored);
                    });
                } else if (self->request.size() < 16384) {
                    self->read();
                }
            });
        }
    };

public:
    RejectedWebSocketServer() : acceptor(context) {
        Net::Resolver resolver(context);
        Net::Endpoint endpoint = *resolver.resolve("127.0.0.1", "0").begin();
        acceptor.open(endpoint.protocol());
        acceptor.bind(endpoint);
        acceptor.listen();
        endpointUrl = "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
        accept();
        worker = std::jthread([this] {
            context.run();
        });
    }

    ~RejectedWebSocketServer() {
        context.stop();
    }

    const std::string& url() const {
        return endpointUrl;
    }
    unsigned rejectedRequests() const {
        return rejected.load();
    }

private:
    Net::IoContext        context;
    Net::Acceptor         acceptor;
    std::atomic<unsigned> rejected{0};
    std::string           endpointUrl;
    // Join before destroying the context and the state referenced by its callbacks.
    std::jthread worker;

    void accept() {
        acceptor.async_accept([this](std::error_code error, Net::Socket socket) {
            if (error)
                return;
            std::make_shared<Session>(std::move(socket), rejected)->read();
            accept();
        });
    }
};
