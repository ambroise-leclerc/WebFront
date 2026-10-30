/// @brief Socket mock driving a WebLink from the test thread, with injectable write failures.
#pragma once
#include "InlineTaskScope.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace webfront::testing {

/// A sender completed by the test rather than by the network: `start` receives the means to complete or
/// stop the operation, and decides when to use them.
class ManualSender {
public:
    struct Completion {
        std::function<void(std::error_code, std::size_t)> complete;
        std::function<void()>                             stop;
    };
    using sender_concept        = networking::ex::sender_t;
    using completion_signatures = networking::ex::completion_signatures<networking::ex::set_value_t(std::size_t),
                                                                       networking::ex::set_error_t(std::error_code),
                                                                       networking::ex::set_stopped_t()>;

    explicit ManualSender(std::function<void(Completion)> start) : onStart(std::move(start)) {}

    // Completed by the test, or stopped by the stop request of the awaiting task, whichever comes first.
    template <typename Receiver>
    struct Operation {
        using StopToken = networking::ex::stop_token_of_t<networking::ex::env_of_t<Receiver>>;
        struct OnStop {
            Operation* operation;
            void       operator()() noexcept { operation->finish([](Receiver& receiver) { networking::ex::set_stopped(std::move(receiver)); }); }
        };

        Receiver                                                    receiver;
        std::function<void(Completion)>                             onStart;
        std::shared_ptr<bool>                                       finished = std::make_shared<bool>(false);  // outlives the operation
        std::optional<networking::ex::stop_callback_for_t<StopToken, OnStop>> stopCallback;

        Operation(Receiver r, std::function<void(Completion)> start) : receiver(std::move(r)), onStart(std::move(start)) {}

        void start() noexcept {
            auto token = networking::ex::get_stop_token(networking::ex::get_env(receiver));
            if (token.stop_requested()) {
                finish([](Receiver& r) { networking::ex::set_stopped(std::move(r)); });
                return;  // completing may destroy this operation: touch no member afterwards
            }
            stopCallback.emplace(std::move(token), OnStop{this});
            onStart({[this, done = finished](std::error_code error, std::size_t size) {
                         if (*done) return;
                         finish([&](Receiver& r) {
                             if (error) networking::ex::set_error(std::move(r), error);
                             else networking::ex::set_value(std::move(r), size);
                         });
                     },
                     [this, done = finished] {
                         if (!*done) finish([](Receiver& r) { networking::ex::set_stopped(std::move(r)); });
                     }});
        }

        void finish(auto signal) {
            if (*finished) return;
            *finished = true;
            stopCallback.reset();
            signal(receiver);
        }
    };

    template <typename Receiver>
    Operation<Receiver> connect(Receiver receiver) const {
        return Operation<Receiver>(std::move(receiver), onStart);
    }

private:
    std::function<void(Completion)> onStart;
};

/// Reads are injected with receive()/fail(). Writes complete synchronously, inside the calling
/// write, unless deferWrites() queues their completions for completeDeferredWrites().
class InjectableSocket : public networking::SocketBaseMock {
public:
    enum shutdown_type { shutdown_receive, shutdown_send, shutdown_both };

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

    /// The read the WebSocket is waiting for: the test completes it, or the task is stopped.
    static ManualSender read(networking::buffers::MutableBuffer buffer) {
        return ManualSender([buffer](ManualSender::Completion completion) {
            ++readCalls;
            readBuffer     = buffer;
            readCompletion = std::move(completion);
        });
    }

    static void receive(std::span<const std::byte> bytes) {
        REQUIRE(readCompletion.complete);
        REQUIRE(bytes.size() <= readBuffer.size());
        std::copy(bytes.begin(), bytes.end(), static_cast<std::byte*>(readBuffer.data()));
        std::exchange(readCompletion, {}).complete({}, bytes.size());
    }

    /// An error ends the read; no error is a read of zero bytes, the end of the stream.
    static void fail(std::error_code error) {
        REQUIRE(readCompletion.complete);
        std::exchange(readCompletion, {}).complete(error, 0);
    }

    /// Stops a pending read, as the WebSocket does when it is stopped.
    static void reset() {
        if (readCompletion.stop) std::exchange(readCompletion, {}).stop();
        std::lock_guard lock(mutex);
        written.clear();
        writeError = {};
        deferring  = false;
        deferred.clear();
        readCalls  = 0;
    }

    static int readCallCount() { return readCalls; }

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

    /// Where the server first sent these bytes, or npos when it did not.
    static std::size_t writtenAt(std::string_view text) {
        std::lock_guard lock(mutex);
        const auto*     first = reinterpret_cast<const std::byte*>(text.data());
        const auto      found = std::search(written.begin(), written.end(), first, first + text.size());
        return found == written.end() ? std::string_view::npos : static_cast<std::size_t>(found - written.begin());
    }

    static void failWrites(std::error_code error) {
        std::lock_guard lock(mutex);
        writeError = error;
    }

    static void deferWrites(bool defer = true) {
        std::lock_guard lock(mutex);
        deferring = defer;
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

    /// The write of these buffers: complete at once, or when completeDeferredWrites() runs.
    static ManualSender write(const auto& buffers) {
        std::error_code error;
        std::size_t     transferred = 0;
        InjectableSocket socket;
        for (const auto& buffer : buffers) {
            transferred += socket.write_some(buffer, error);
            if (error)
                break;
        }
        return ManualSender([error, transferred](ManualSender::Completion completion) {
            {
                std::lock_guard lock(mutex);
                if (deferring) {
                    deferred.push_back([completion, error, transferred] { completion.complete(error, transferred); });
                    return;
                }
            }
            completion.complete(error, transferred);
        });
    }

private:
    inline static networking::buffers::MutableBuffer readBuffer;
    inline static ManualSender::Completion           readCompletion;
    inline static std::mutex                         mutex;
    inline static std::vector<std::byte>             written;
    inline static std::error_code                    writeError;
    inline static bool                               deferring = false;
    inline static std::vector<std::function<void()>> deferred;
    inline static int                                readCalls  = 0;
};

class InjectableNetworking : public networking::NetworkingMock {
public:
    using Socket = InjectableSocket;

    static ManualSender Receive(Socket&, MutableBuffer buffer) { return InjectableSocket::read(buffer); }
    static ManualSender Send(Socket&, const auto& buffers) { return InjectableSocket::write(buffers); }
};

/// Mask a browser-to-server frame: by default a binary message in one frame (FIN set, opcode 2).
inline std::vector<std::byte> clientFrame(std::span<const std::byte> payload, std::byte finAndOpcode = std::byte{0x82}) {
    REQUIRE(payload.size() < 126);
    constexpr std::array<std::byte, 4> mask{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    std::vector<std::byte>             frame{finAndOpcode, std::byte{static_cast<std::uint8_t>(0x80u | payload.size())}};
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
