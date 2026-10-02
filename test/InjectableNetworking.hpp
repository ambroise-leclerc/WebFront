/// @brief Socket mock driving a WebLink from the test thread, with injectable write failures.
#pragma once
#include <catch2/catch_test_macros.hpp>
#include <http/WebSocket.hpp>
#include <networking/NetworkingMock.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace webfront::testing {

/// Reads are injected with receive()/fail(). Writes complete synchronously, inside the calling
/// write, unless deferWrites() queues their completions for completeDeferredWrites().
class InjectableSocket : public networking::SocketBaseMock {
public:
    enum shutdown_type { shutdown_receive, shutdown_send, shutdown_both };

    void async_read_some(auto buffer, auto completion) {
        readBuffer  = buffer;
        readHandler = std::move(completion);
    }

    std::size_t write_some(auto input, std::error_code& error) {
        std::lock_guard lock(mutex);
        if (writeError) {
            error = writeError;
            return 0;
        }
        const auto* first = static_cast<const std::byte*>(input.data());
        written.insert(written.end(), first, first + input.size());
        return input.size();
    }

    void close() {}
    void shutdown(shutdown_type) {}

    static void receive(std::span<const std::byte> bytes) {
        REQUIRE(readHandler);
        REQUIRE(bytes.size() <= readBuffer.size());
        std::copy(bytes.begin(), bytes.end(), static_cast<std::byte*>(readBuffer.data()));
        auto handler = std::move(readHandler);
        handler({}, bytes.size());
    }

    static void fail(std::error_code error) {
        REQUIRE(readHandler);
        auto handler = std::move(readHandler);
        handler(error, 0);
    }

    static void reset() {
        std::lock_guard lock(mutex);
        readHandler = {};
        written.clear();
        writeError = {};
        deferring  = false;
        deferred.clear();
    }

    static bool hasWrittenData() {
        std::lock_guard lock(mutex);
        return !written.empty();
    }

    /// True when the server sent these bytes, e.g. the text of a debugLog command.
    static bool wrote(std::string_view text) {
        std::lock_guard lock(mutex);
        const auto*     first = reinterpret_cast<const std::byte*>(text.data());
        return std::search(written.begin(), written.end(), first, first + text.size()) != written.end();
    }

    static void failWrites(std::error_code error) {
        std::lock_guard lock(mutex);
        writeError = error;
    }

    static void deferWrites() {
        std::lock_guard lock(mutex);
        deferring = true;
    }

    /// Run queued completions on the calling thread, outside the write that queued them.
    static std::size_t completeDeferredWrites() {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard lock(mutex);
            pending.swap(deferred);
        }
        for (auto& completion : pending)
            completion();
        return pending.size();
    }

    /// Queue a completion when deferring; otherwise run it immediately.
    static void complete(std::function<void()> completion) {
        {
            std::lock_guard lock(mutex);
            if (deferring) {
                deferred.push_back(std::move(completion));
                return;
            }
        }
        completion();
    }

private:
    inline static networking::buffers::MutableBuffer                readBuffer;
    inline static std::function<void(std::error_code, std::size_t)> readHandler;
    inline static std::mutex                                        mutex;
    inline static std::vector<std::byte>                            written;
    inline static std::error_code                                   writeError;
    inline static bool                                              deferring = false;
    inline static std::vector<std::function<void()>>                deferred;
};

class InjectableNetworking : public networking::NetworkingMock {
public:
    using Socket = InjectableSocket;

    template <typename WriteHandler>
    static void AsyncWrite(Socket socket, auto buffers, WriteHandler handler) {
        std::error_code error;
        std::size_t     transferred = 0;
        for (const auto& buffer : buffers) {
            transferred += socket.write_some(buffer, error);
            if (error)
                break;
        }
        Socket::complete([handler = std::move(handler), error, transferred]() mutable {
            handler(error, transferred);
        });
    }
};

/// Mask a browser-to-server binary frame.
inline std::vector<std::byte> clientFrame(std::span<const std::byte> payload) {
    REQUIRE(payload.size() < 126);
    constexpr std::array<std::byte, 4> mask{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    std::vector<std::byte>             frame{std::byte{0x82}, std::byte{static_cast<std::uint8_t>(0x80u | payload.size())}};
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t index = 0; index < payload.size(); ++index)
        frame.push_back(payload[index] ^ mask[index % mask.size()]);
    return frame;
}

template <typename Net>
std::vector<std::byte> messagePayload(const websocket::Frame<Net>& frame) {
    std::vector<std::byte> payload;
    const auto             buffers = frame.toBuffers();
    for (auto buffer = std::next(buffers.begin()); buffer != buffers.end(); ++buffer) {
        const auto* first = static_cast<const std::byte*>(buffer->data());
        payload.insert(payload.end(), first, first + buffer->size());
    }
    return payload;
}

}  // namespace webfront::testing
