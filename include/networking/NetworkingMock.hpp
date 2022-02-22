/// @date 26/01/2022 11:38:14
/// @author Ambroise Leclerc
/// @brief a Networking mock implementation for testing purposes
#pragma once
#include "BasicNetworking.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <system_error>

namespace webfront::networking {

class SocketMock {
public:
    inline static std::array<std::byte, 8192> debugBuffer;
    inline static size_t bufferIndex = 0;
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

    void close() {}
};

class NetworkingMock : public BasicNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;
    using Socket = SocketMock;

    template<typename WriteHandler>
    static void AsyncWrite(Socket socket, auto buffers, WriteHandler writeHandler) {

        auto writeRes = std::async(std::launch::async, [&](){
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
