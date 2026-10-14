/// @date 26/01/2022 11:38:14
/// @author Ambroise Leclerc
/// @brief a Networking mock implementation for testing purposes
#pragma once
#include "BasicNetworking.hpp"
#include "Tasks.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <list>
#include <string_view>
#include <system_error>

namespace webfront::networking {

/// A sender of a T which only ever stops: the operation it stands for never completes.
template <typename T>
struct StoppedSender {
    using sender_concept        = ex::sender_t;
    using completion_signatures = ex::completion_signatures<ex::set_value_t(T), ex::set_stopped_t()>;

    template <typename Receiver>
    struct Operation {
        Receiver receiver;
        void start() noexcept { ex::set_stopped(std::move(receiver)); }
    };

    template <typename Receiver>
    Operation<Receiver> connect(Receiver receiver) const {
        return {std::move(receiver)};
    }
};

class IoContextMock {
public:
    void run() {}
    void run_one() {}
    void stop() {}
    void restart() {}
};

class EndpointMock {
    std::string originalAddress, originalPort;

public:
    EndpointMock(std::string_view address, std::string_view port) : originalAddress(address), originalPort(port) {}
    auto protocol() const { return originalPort; }
};

class SocketBaseMock {
public:
    // auto reuse_address(bool reuse) { return reuse ? "SO_REUSEADDR:1" : "SO_REUSEADDR:0"; }
    struct reuse_address {
        bool isSet;

        explicit reuse_address(bool reuse)
            : isSet(reuse) {}

        bool value() const { return isSet; }
    };
    void set_option(reuse_address option) { log::debug("SocketBaseMock::set_option(reuse_address({}))", option.value()); }
    void bind(const EndpointMock&) { log::debug("SocketBaseMock::bind()"); }
    void listen(int backlog = max_connections) { log::debug("SocketBaseMock::listen({})", backlog); }

public:
    static const int max_connections = 2;
};

class SocketMock : public SocketBaseMock {
public:
    inline static std::array<std::byte, 8192> debugBuffer;
    inline static size_t bufferIndex = 0;

    enum shutdown_type { shutdown_receive, shutdown_send, shutdown_both };

public:
    SocketMock() {
        debugBuffer.fill({});
        bufferIndex = 0;
    }

    size_t write_some(auto inputBuffer, std::error_code&) {
        std::copy_n(reinterpret_cast<const std::byte*>(inputBuffer.data()), inputBuffer.size(), &debugBuffer[bufferIndex]);
        bufferIndex += inputBuffer.size();
        return inputBuffer.size();
    }

    void close() { log::debug("SocketMock::close()"); }
    void shutdown(shutdown_type type) { log::debug("SocketMock::shutdown({})", static_cast<int>(type)); }
};

class AcceptorMock : public SocketBaseMock {
    bool isOpen = false;

public:
    AcceptorMock(IoContextMock){};
    void open(auto protocol) {
        log::debug("AcceptorMock::open({})", protocol);
        isOpen = true;
    }
    bool is_open() const { return isOpen; }
};

class NetworkingMock : public BasicNetworking<> {
public:
    using Acceptor = AcceptorMock;
    using Endpoint = EndpointMock;
    using IoContext = IoContextMock;
    using Socket = SocketMock;
    using super::ConstBuffer;
    using super::MutableBuffer;

    static EndpointMock Resolve(std::string_view address, std::string_view port) { return {address, port}; }
    static auto Scheduler(auto&) { return ex::inline_scheduler{}; }

    /// Nothing is ever received or accepted: the tasks awaiting these operations end at once.
    static StoppedSender<std::size_t> Receive(Socket&, MutableBuffer) { return {}; }
    static StoppedSender<Socket> Accept(auto&) { return {}; }
    static auto Send(Socket& socket, const auto& buffers) {
        std::error_code ec;
        std::size_t bytesTransferred = 0;
        for (const auto& buffer : buffers) bytesTransferred += socket.write_some(buffer, ec);
        return ex::just(bytesTransferred);
    }
};

} // namespace webfront::networking
