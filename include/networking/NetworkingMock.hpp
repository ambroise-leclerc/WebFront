/// @date 26/01/2022 11:38:14
/// @author Ambroise Leclerc
/// @brief a Networking mock implementation for testing purposes
#pragma once
#include "BasicNetworking.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <list>
#include <string_view>
#include <system_error>

namespace webfront::networking {

class IoContextMock {};

class EndpointMock {
    std::string originalAddress, originalPort;

public:
    EndpointMock(std::string_view address, std::string_view port) : originalAddress(address), originalPort(port) {}
    auto protocol() { return originalPort; }
};

class SocketBaseMock {
public:
    // auto reuse_address(bool reuse) { return reuse ? "SO_REUSEADDR:1" : "SO_REUSEADDR:0"; }
    struct reuse_address {
        bool isSet;
        bool value() { return isSet; }
    };
    void set_option(reuse_address option) { log::debug("SocketBaseMock::set_option(reuse_address({}))", option.value()); }
    void bind(const EndpointMock&) { log::debug("SocketBaseMock::bind()"); }
    void listen(int backlog = max_connections) { log::debug("SocketBaseMock::listen({})", backlog); }

public:
    static const int max_connections = 2;
};

class ResolverMock {
public:
    ResolverMock(IoContextMock){};
    std::list<EndpointMock> resolve(std::string_view address, std::string_view port) {
        std::list<EndpointMock> endpoints;
        endpoints.push_back(EndpointMock{address, port});
        return endpoints;
    }
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

    void async_read_some(auto /*Buffer*/, auto /*completionFunction*/) {}

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
    void async_accept(std::function<void(std::error_code, SocketMock)>) { log::debug("AcceptorMock::aync_accept()"); }
    bool is_open() const { return isOpen; }
};

class NetworkingMock : public BasicNetworking<> {
public:
    using Acceptor = AcceptorMock;
    using Endpoint = EndpointMock;
    using IoContext = IoContextMock;
    using Resolver = ResolverMock;
    using Socket = SocketMock;
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename WriteHandler>
    static void AsyncWrite(Socket socket, auto buffers, WriteHandler writeHandler) {
        auto writeRes = std::async(std::launch::async, [&]() {
            std::error_code ec;
            size_t bytesTransferred = 0;
            for (const auto& buffer : buffers) {
                bytesTransferred += socket.write_some(buffer, ec);
                if (ec) {
                    writeHandler(ec, bytesTransferred);
                    return;
                }
            }

            writeHandler(ec, bytesTransferred);
        });
    }

    struct Error {
        static inline const auto OperationAborted = std::make_error_code(std::errc::operation_canceled);
    };
};

} // namespace webfront::networking
