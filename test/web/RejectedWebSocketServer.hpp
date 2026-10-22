#pragma once

#include <webfront/networking/Tasks.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>

#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <thread>

// A test-owned endpoint that accepts HTTP requests but always refuses WebSocket upgrades.
// Binding port zero keeps the initial-connection-failure tests independent of other services.
class RejectedWebSocketServer {
    using Net = webfront::networking::TCPStdnet;
    using Task = webfront::networking::Task;

public:
    RejectedWebSocketServer() : acceptor(context) {
        const Net::Endpoint endpoint = Net::Resolve("127.0.0.1", "0");
        acceptor.open(endpoint.protocol());
        acceptor.bind(endpoint);
        acceptor.listen();
        endpointUrl = "ws://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
        spawn(acceptConnections());
        worker = std::jthread([this] {
            context.run();
        });
    }

    // Stopping cancels the pending operations; the worker returns once their tasks have ended.
    ~RejectedWebSocketServer() {
        tasks.requestStop();
        worker.join();
        tasks.drain(context);
    }

    const std::string& url() const {
        return endpointUrl;
    }
    unsigned rejectedRequests() const {
        return rejected.load();
    }

private:
    static constexpr std::string_view response{"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};

    Net::IoContext        context;
    Net::Acceptor         acceptor;
    webfront::networking::TaskScope tasks;
    std::atomic<unsigned> rejected{0};
    std::string           endpointUrl;
    std::jthread          worker;

    void spawn(Task task) {
        webfront::networking::Spawn(tasks, Net::Scheduler(acceptor), std::move(task));
    }

    Task acceptConnections() {
        for (;;) spawn(reject(co_await Net::Accept(acceptor)));
    }

    Task reject(Net::Socket socket) {
        std::array<char, 1024> buffer{};
        std::string            request;
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const auto received = co_await Net::Receive(socket, Net::Buffer(buffer));
            if (received == 0)
                co_return;
            request.append(buffer.data(), received);
        }
        ++rejected;
        co_await Net::Send(socket, Net::Buffer(response));
    }
};
