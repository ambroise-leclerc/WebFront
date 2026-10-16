/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief WebSocket protocol implementation - RFC6455
#pragma once
#include "BuffersPolicy.hpp"
#include "Encodings.hpp"
#include "../tooling/HexDump.hpp"
#include "../networking/Tasks.hpp"
#include "../tooling/Logger.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <system_error>

namespace webfront::websocket {
using Handle = uint32_t;

struct Header {
    static constexpr size_t maxHeaderSize = 14;
    std::array<std::byte, maxHeaderSize> raw{};

    enum class Opcode {
        continuation,
        text,
        binary,
        reserved1,
        reserved2,
        reserved3,
        reserved4,
        reserved5,
        connectionClose,
        ping,
        pong,
        ctrl1,
        ctrl2,
        ctrl3,
        ctrl4,
        ctrl5,
    };

    [[nodiscard]] bool FIN() const { return test(0, 7); }
    [[nodiscard]] bool RSV1() const { return test(0, 6); }
    [[nodiscard]] bool RSV2() const { return test(0, 5); }
    [[nodiscard]] bool RSV3() const { return test(0, 4); }
    [[nodiscard]] Opcode opcode() const { return static_cast<Opcode>(std::to_integer<uint8_t>(raw[0] & std::byte(0b1111))); }
    [[nodiscard]] bool MASK() const { return test(1, 7); }
    [[nodiscard]] uint8_t payloadLenField() const { return std::to_integer<uint8_t>(raw[1] & std::byte(0b1111111)); }
    [[nodiscard]] uint64_t extendedLenField() const {
        if (payloadLenField() < 126) return 0;
        auto s = [this](size_t i, uint8_t shift = 0) constexpr { return std::to_integer<uint64_t>(raw[i]) << shift; };
        return payloadLenField() == 126 ? s(2, 8) | s(3) : s(2, 56) | s(3, 48) | s(4, 40) | s(5, 32) | s(6, 24) | s(7, 16) | s(8, 8) | s(9, 0);
    }

    [[nodiscard]] size_t headerSize() const {
        if (payloadLenField() < 126) return MASK() ? 6 : 2;
        if (payloadLenField() == 126) return MASK() ? 8 : 4;
        return MASK() ? 14 : 10;
    }

    [[nodiscard]] std::array<std::byte, 4> maskingKey() const {
        if (!MASK()) return {};
        auto index = headerSize() - 4;
        return {raw[index], raw[index + 1], raw[index + 2], raw[index + 3]};
    }

    [[nodiscard]] uint64_t payloadSize() const {
        auto len = payloadLenField();
        return len < 126 ? len : extendedLenField();
    }

    [[nodiscard]] uint64_t getFrameSize() const { return payloadSize() + headerSize(); }

    void dump() const {
        log::debug("FIN:{} RSV1:{} RSV2:{} RSV3:{} opcode:{}", FIN(), RSV1(), RSV2(), RSV3(), static_cast<int>(opcode()));
        log::debug("-> MASK:{} payloadLenField:{} extendedLenField:{}", MASK(), +payloadLenField(), extendedLenField());
        auto k = maskingKey();
        log::debug("-> maskingKey:0x{:02x}{:02x}{:02x}{:02x} getPayloadSize:{} headerSize:{}", std::to_integer<int>(k[0]), std::to_integer<int>(k[1]),
                   std::to_integer<int>(k[2]), std::to_integer<int>(k[3]), payloadSize(), headerSize());
    }

    /// @return true if first 'size' bytes constitute a complete header
    [[nodiscard]] bool isComplete(size_t size) const {
        if (size < 2) return false;
        return size >= headerSize();
    }

    void setFIN(bool set) {
        if (set)
            raw[0] |= std::byte(1 << 7);
        else
            raw[0] &= std::byte(0b1111111);
    }

    void setOpcode(Opcode code) {
        raw[0] &= std::byte(0b11110000);
        raw[0] |= static_cast<std::byte>(code);
    }

    void setPayloadSize(size_t size) {
        raw[1] = (raw[1] & std::byte(0b10000000)) | std::byte(size < 126 ? size : size < 65536 ? 126 : 127);
        size_t len = size < 126 ? 0 : size < 65536 ? 2 : 8;
        for (size_t i = 0; i < len; ++i) raw[2 + i] = std::byte(size >> (8 * (len - i - 1)));
    }

protected:
    bool test(size_t index, uint8_t bit) const { return std::to_integer<bool>(raw[index] & std::byte(1 << bit)); }
};

struct CloseEvent {
    uint16_t status;
    std::string reason;
};

template<typename Net>
struct Frame : public Header {
    Frame(std::string_view text) {
        setFIN(true);
        setOpcode(Opcode::text);
        setPayloadSize(text.size());
        borrowedBuffers.emplace_back(reinterpret_cast<const std::byte*>(text.data()), text.size());
    }

    Frame(std::span<const std::byte> dataHead, std::span<const std::byte> dataNext = {}) {
        setFIN(true);
        setOpcode(Opcode::binary);
        setPayloadSize(dataHead.size() + dataNext.size());
        borrowedBuffers.emplace_back(dataHead);
        if (!dataNext.empty()) borrowedBuffers.emplace_back(dataNext);
    }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = default;
    Frame& operator=(Frame&&) = default;

    [[nodiscard]] size_t size() const { return payloadSize(); };

    std::vector<typename Net::ConstBuffer> toBuffers() const {
        std::vector<typename Net::ConstBuffer> buffers;
        auto appendPayload = [&buffers](const auto& source) {
            for (const auto& buffer : source) buffers.emplace_back(buffer.data(), buffer.size());
        };
        // freeze() moves the payload from borrowedBuffers into ownedBuffers, so only one of the two holds it.
        buffers.reserve((ownedBuffers.empty() ? borrowedBuffers.size() : ownedBuffers.size()) + 1);
        buffers.emplace_back(raw.data(), headerSize());
        if (ownedBuffers.empty())
            appendPayload(borrowedBuffers);
        else
            appendPayload(ownedBuffers);
        return buffers;
    }

    void freeze() {
        if (borrowedBuffers.empty()) return;
        ownedBuffers.clear();
        ownedBuffers.reserve(borrowedBuffers.size());
        for (const auto buffer : borrowedBuffers) ownedBuffers.emplace_back(buffer.begin(), buffer.end());
        borrowedBuffers.clear();
    }

    /// @return size of added buffer
    size_t addBuffer(std::span<const std::byte> buffer) {
        log::debug("frame::addBuffer {}", utils::hexDump(buffer));
        setPayloadSize(payloadSize() + buffer.size());
        borrowedBuffers.emplace_back(buffer);
        return buffer.size();
    }

private:
    std::vector<std::span<const std::byte>> borrowedBuffers;
    std::vector<std::vector<std::byte>> ownedBuffers;
};

class FrameDecoder {
public:
    Header::Opcode frameType;
    bool finalFrame = true;  ///< FIN bit: false for every fragment of a message but its last

public:
    FrameDecoder() : payloadBuffer(sizeof(Header)) { reset(); }
    std::span<const std::byte> payload() const { return std::span(payloadBuffer.data(), payloadSize); }
    [[nodiscard]] size_t consumed() const { return consumedSize; }
    /// True when the last frame's header announced more than its budget: nothing was reserved for it.
    [[nodiscard]] bool rejected() const { return oversized; }

    // Parses some incoming data and tries to decode it.
    // @return true if the frame is complete, false if it needs more data
    bool parse(std::span<const std::byte> buffer) {
        return parse(buffer, [](Header::Opcode) { return std::numeric_limits<size_t>::max(); });
    }

    // Same, rejecting a frame whose header announces more than maxPayload(opcode) bytes before
    // reserving them: parse() then returns false and rejected() true.
    template<typename Budget>
    bool parse(std::span<const std::byte> buffer, Budget&& maxPayload) {
        consumedSize = 0;
        switch (state) {
        case DecodingState::starting:
            return parseStart(buffer, maxPayload);
        case DecodingState::partialHeader:
            return parsePartialHeader(buffer, maxPayload);
        case DecodingState::decodingPayload:
            return parsePayload(buffer);
        }
        return false;
    }

    void reset() {
        maskIndex = 0;
        headerBufferParser = 0;
        payloadBuffer.clear();
        state = DecodingState::starting;
        consumedSize = 0;
        oversized = false;
    }

private:
    // A frame whose header may be complete in `buffer`; otherwise its first bytes are kept.
    template<typename Budget>
    bool parseStart(std::span<const std::byte> buffer, Budget& maxPayload) {
        const auto* header = reinterpret_cast<const Header*>(buffer.data());
        if (!header->isComplete(buffer.size())) {
            consumedSize = bufferizeHeaderData(buffer);
            state        = DecodingState::partialHeader;
            return false;
        }
        header->dump();
        consumedSize = header->headerSize();
        return startPayload(*header, buffer.subspan(consumedSize), maxPayload);
    }

    template<typename Budget>
    bool parsePartialHeader(std::span<const std::byte> buffer, Budget& maxPayload) {
        consumedSize = bufferizeHeaderData(buffer);
        if (!headerBuffer.isComplete(headerBufferParser))
            return false;
        return startPayload(headerBuffer, buffer.subspan(consumedSize), maxPayload);
    }

    // Decodes a complete header, then the start of its payload found in `rest`.
    template<typename Budget>
    bool startPayload(const Header& header, std::span<const std::byte> rest, Budget& maxPayload) {
        if (!decodeHeader(header, maxPayload))
            return false;
        if (decodePayload(rest.first(std::min(rest.size(), payloadSize))) == payloadSize)
            return true;
        state = DecodingState::decodingPayload;
        return false;
    }

    bool parsePayload(std::span<const std::byte> buffer) {
        const auto remaining = payloadSize - payloadBuffer.size();
        decodePayload(buffer.first(std::min(remaining, buffer.size())));
        return consumedSize == remaining;
    }

    // Rejects a frame announcing more than maxPayload(opcode) bytes before reserving them.
    template<typename Budget>
    bool decodeHeader(const Header& header, Budget& maxPayload) {
        payloadSize = header.payloadSize();
        headerSize  = header.headerSize();
        mask        = header.maskingKey();
        frameType   = header.opcode();
        finalFrame  = header.FIN();
        oversized   = payloadSize > maxPayload(frameType);
        if (!oversized)
            payloadBuffer.reserve(payloadSize);
        return !oversized;
    }

    size_t decodePayload(std::span<const std::byte> encoded) {
        for (auto in : encoded)
            payloadBuffer.push_back(in ^ mask[maskIndex++ % 4]);
        consumedSize += encoded.size();
        return payloadBuffer.size();
    }

    size_t bufferizeHeaderData(std::span<const std::byte> input) {
        for (size_t index = 0; index < input.size(); ++index) {
            headerBuffer.raw[headerBufferParser++] = input[index];
            if (headerBuffer.isComplete(headerBufferParser))
                return index + 1;
        }
        return input.size();
    }

    enum class DecodingState { starting, partialHeader, decodingPayload } state;
    std::vector<std::byte> payloadBuffer;
    Header headerBuffer;
    size_t headerBufferParser;
    size_t payloadSize, headerSize;
    size_t consumedSize;
    bool oversized = false;
    std::array<std::byte, 4> mask;
    uint8_t maskIndex;
};

template<typename Net, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
class WebSocket : public std::enable_shared_from_this<WebSocket<Net, Policy>> {
    typename Net::Socket socket;

public:
    /// WebSocket instances must be heap-allocated and owned via shared_ptr: the tasks reading and writing
    /// the socket keep the object alive, even if the owner (e.g. WebLink) is destroyed while they run.
    /// Call start() once message handlers are attached.
    [[nodiscard]] static std::shared_ptr<WebSocket> create(typename Net::Socket netSocket) {
        return std::shared_ptr<WebSocket>(new WebSocket(std::move(netSocket)));
    }
    WebSocket(const WebSocket&) = delete;
    WebSocket(WebSocket&&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;
    WebSocket& operator=(WebSocket&&) = delete;
    ~WebSocket() { log::debug("WebSocket destructor"); }

    void start() {
        started = true;
        spawn(readFrames(this->shared_from_this()));
    }

    /// Thread-safe. Cancels the pending read and write; the socket closes once their tasks have ended.
    void stop() {
        if (claimClose())
            stopSource.request_stop();
    }

    void onMessage(std::function<void(std::string_view)>&& handler) { textHandler = std::move(handler); }
    void onMessage(std::function<void(std::span<const std::byte>)>&& handler) { binaryHandler = std::move(handler); }
    void onClose(std::function<void(CloseEvent)>&& handler) { closeHandler = std::move(handler); }
    /// Called on every failed write, before its diagnostic and even after stop(): frames queued
    /// before stopping still fail, and their producer must be detached rather than fed the error.
    void onWriteError(std::function<void(std::error_code)>&& handler) { writeErrorHandler = std::move(handler); }
    void write(std::string_view text) { writeData(Frame<Net>(text)); }
    void write(std::span<const std::byte> data) { writeData(Frame<Net>(data)); }
    void write(std::span<const std::byte> data, std::span<const std::byte> data2) { writeData(Frame<Net>(data, data2)); }
    void write(Frame<Net> frame) { writeData(std::move(frame)); }
    /// Queue a droppable frame (a diagnostic) only while fewer than maxPending frames await the
    /// network, which may stop reading; false when refused. Other writes are never refused.
    [[nodiscard]] bool tryWrite(std::span<const std::byte> data, std::span<const std::byte> data2, std::size_t maxPending) {
        return enqueue(Frame<Net>(data, data2), maxPending);
    }
    /// Frames queued and not yet completed by the network, including the one being written.
    [[nodiscard]] std::size_t pendingWrites() const {
        std::lock_guard lock(writeMutex);
        return writeQueue.size();
    }

private:
    std::array<std::byte, Policy::receptionBufferSize> readBuffer;
    FrameDecoder decoder;
    std::function<void(std::string_view)> textHandler;
    std::function<void(std::span<const std::byte>)> binaryHandler;
    std::function<void(CloseEvent)> closeHandler;
    std::function<void(std::error_code)> writeErrorHandler;
    mutable std::mutex writeMutex;
    std::deque<std::shared_ptr<Frame<Net>>> writeQueue;  // the first frame is the one being written
    networking::ex::inplace_stop_source stopSource;
    std::atomic_bool started;
    // A message sent in several frames (RFC 6455, 5.4), as browsers do with large messages: its
    // type and the payloads received so far, at most http::maxMessageSize<Policy>() bytes
    // (payloadBudget()).
    std::optional<Header::Opcode> fragmentedType;
    std::vector<std::byte> fragments;

private:
    explicit WebSocket(typename Net::Socket netSocket) : socket(std::move(netSocket)), started(false) {
        log::debug("WebSocket constructor");
    }

    bool claimClose() { return started.exchange(false); }

    void spawn(networking::Task task) { networking::Spawn(Net::Scheduler(socket), stopSource.get_token(), std::move(task)); }

    // The tasks take a shared_ptr to this object: it keeps the WebSocket alive while they run.
    networking::Task readFrames(std::shared_ptr<WebSocket>) {
        std::error_code failure;
        try {
            while (started) {
                const auto received = co_await Net::Receive(socket, Net::Buffer(readBuffer));
                if (received == 0) {
                    failure = std::make_error_code(std::errc::connection_reset);  // the browser closed the connection
                    break;
                }
                processFrames(std::span(readBuffer.data(), received));
            }
        }
        catch (const std::system_error& error) {
            failure = error.code();
        }
        if (failure) closeOnError(failure);
    }

    void processFrames(std::span<const std::byte> received) {
        while (!received.empty() && started) {
            const auto frameComplete = decoder.parse(received, [this](Header::Opcode type) { return payloadBudget(type); });
            if (decoder.rejected())
                return rejectFrame();
            received = received.subspan(decoder.consumed());
            if (!frameComplete)
                break;
            dispatchFrame();
            decoder.reset();
        }
    }

    void dispatchFrame() {
        switch (decoder.frameType) {
            case Header::Opcode::text:
            case Header::Opcode::binary:
                startMessage(decoder.payload());
                break;
            case Header::Opcode::continuation:
                continueMessage(decoder.payload());
                break;
            default:
                dispatchControlFrame();
        }
    }

    // Control frames (close, ping, pong) and reserved opcodes: none may be fragmented (RFC 6455, 5.5).
    void dispatchControlFrame() {
        if (!decoder.finalFrame)
            return closeOnError(std::make_error_code(std::errc::protocol_error));
        if (decoder.frameType != Header::Opcode::connectionClose) {
            log::debug("Unhandled frameType");
            return;
        }
        if (closeHandler)
            closeHandler(CloseEvent{});
        stop();
    }

    // Control frames (close, ping, pong) carry at most 125 bytes (RFC 6455, 5.5), whatever maxMessageSize.
    static constexpr size_t maxControlPayload = 125;

    static bool isControl(Header::Opcode type) { return type >= Header::Opcode::connectionClose; }

    // The largest payload a frame may announce: 125 bytes for a control frame, what is left of
    // maxMessageSize for a continuation, all of it for another frame. Checked on the header, before
    // the payload is buffered.
    size_t payloadBudget(Header::Opcode type) const {
        constexpr auto limit = http::maxMessageSize<Policy>();
        if (isControl(type))
            return maxControlPayload;
        return type == Header::Opcode::continuation ? limit - fragments.size() : limit;
    }

    // A frame announcing more than its budget: an oversized control frame breaks the protocol, an
    // oversized message exceeds maxMessageSize.
    void rejectFrame() {
        closeOnError(std::make_error_code(isControl(decoder.frameType) ? std::errc::protocol_error : std::errc::message_size));
    }

    // The first frame of a text or binary message.
    void startMessage(std::span<const std::byte> data) {
        if (fragmentedType)
            return closeOnError(std::make_error_code(std::errc::protocol_error));  // the previous message is unfinished
        if (decoder.finalFrame)
            return dispatchMessage(decoder.frameType, data);
        fragmentedType = decoder.frameType;
        fragments.assign(data.begin(), data.end());
    }

    void continueMessage(std::span<const std::byte> data) {
        if (!fragmentedType)
            return closeOnError(std::make_error_code(std::errc::protocol_error));  // no message to continue
        fragments.insert(fragments.end(), data.begin(), data.end());
        if (!decoder.finalFrame)
            return;
        const auto message = std::exchange(fragments, {});
        dispatchMessage(*std::exchange(fragmentedType, std::nullopt), message);
    }

    void dispatchMessage(Header::Opcode type, std::span<const std::byte> data) {
        if (type == Header::Opcode::text) {
            if (textHandler)
                textHandler(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        }
        else if (binaryHandler)
            binaryHandler(data);
    }

    void closeOnError(std::error_code ec) {
        if (!claimClose())
            return;
        if (ec != std::errc::operation_canceled)
            log::error("WebSocket I/O error: {}:{}", ec.value(), ec.message());
        // Report the closure before stopping: it detaches the producers, whose writes would fail once stopped.
        if (closeHandler)
            closeHandler(CloseEvent{static_cast<uint16_t>(ec.value()), ec.message()});
        stopSource.request_stop();
    }

    void writeData(Frame<Net> frame) { static_cast<void>(enqueue(std::move(frame), std::numeric_limits<std::size_t>::max())); }

    bool enqueue(Frame<Net> frame, std::size_t maxPending) {
        frame.freeze();
        auto pendingFrame = std::make_shared<Frame<Net>>(std::move(frame));
        {
            std::lock_guard lock(writeMutex);
            if (writeQueue.size() >= maxPending) return false;
            writeQueue.push_back(std::move(pendingFrame));
            if (writeQueue.size() > 1) return true;  // the running writeFrames() task will get to it
        }
        spawn(writeFrames(this->shared_from_this()));
        return true;
    }

    // Writes the queued frames in order and ends when the queue is empty.
    networking::Task writeFrames(std::shared_ptr<WebSocket>) {
        try {
            for (auto frame = nextFrame(false); frame; frame = nextFrame(true)) {
                // A stopped WebSocket cancels the write: it fails like a write on a closed connection.
                co_await networking::ex::stopped_as_error(Net::Send(socket, frame->toBuffers()), std::make_error_code(std::errc::operation_canceled));
            }
        }
        catch (const std::system_error& error) {
            {
                std::lock_guard lock(writeMutex);
                writeQueue.clear();
            }
            onWriteFailed(error.code());
        }
    }

    // The frame to write, after having removed the written one if `written`; null when none remains.
    std::shared_ptr<Frame<Net>> nextFrame(bool written) {
        std::lock_guard lock(writeMutex);
        if (written) writeQueue.pop_front();
        return writeQueue.empty() ? nullptr : writeQueue.front();
    }

    /// The producer is detached before the diagnostic; the flag only gates the close sequence.
    void onWriteFailed(std::error_code ec) {
        if (writeErrorHandler) writeErrorHandler(ec);
        closeOnError(ec);
    }
};

[[nodiscard]] inline std::string getHashedSecKey(std::string key) {
    constexpr auto rfc6455Guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64::encodeInNetworkOrder(crypto::sha1(key + rfc6455Guid));
}                    

} // namespace webfront::websocket
