/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief WebSocket protocol implementation - RFC6455
#pragma once
#include "../tooling/HexDump.hpp"
#include "../tooling/Logger.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <span>

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

    bool FIN() const { return test(0, 7); }
    bool RSV1() const { return test(0, 6); }
    bool RSV2() const { return test(0, 5); }
    bool RSV3() const { return test(0, 4); }
    Opcode opcode() const { return static_cast<Opcode>(std::to_integer<uint8_t>(raw[0] & std::byte(0b1111))); }
    bool MASK() const { return test(1, 7); }
    uint8_t payloadLenField() const { return std::to_integer<uint8_t>(raw[1] & std::byte(0b1111111)); }
    uint64_t extendedLenField() const {
        auto s = [this](size_t i, uint8_t shift = 0) constexpr { return std::to_integer<uint64_t>(raw[i]) << shift; };
        return payloadLenField() == 126 ? s(2, 8) | s(3) : s(2, 56) | s(3, 48) | s(4, 40) | s(5, 32) | s(6, 24) | s(7, 16) | s(8, 8) | s(9, 0);
    }

    size_t headerSize() const {
        if (payloadLenField() < 126) return MASK() ? 6 : 2;
        if (payloadLenField() == 126) return MASK() ? 8 : 4;
        return MASK() ? 14 : 10;
    }

    std::array<std::byte, 4> maskingKey() const {
        if (!MASK()) return {};
        auto index = headerSize() - 4;
        return {raw[index], raw[index + 1], raw[index + 2], raw[index + 3]};
    }

    uint64_t payloadSize() const {
        auto len = payloadLenField();
        return len < 126 ? len : extendedLenField();
    }

    uint64_t getFrameSize() const { return payloadSize() + headerSize(); }

    void dump() const {
        log::debug("FIN:{} RSV1:{} RSV2:{} RSV3:{} opcode:{}", FIN(), RSV1(), RSV2(), RSV3(), static_cast<int>(opcode()));
        log::debug("-> MASK:{} payloadLenField:{} extendedLenField:{}", MASK(), +payloadLenField(), extendedLenField());
        auto k = maskingKey();
        log::debug("-> maskingKey:0x{:02x}{:02x}{:02x}{:02x} getPayloadSize:{} headerSize:{}", std::to_integer<int>(k[0]), std::to_integer<int>(k[1]),
                   std::to_integer<int>(k[2]), std::to_integer<int>(k[3]), payloadSize(), headerSize());
    }

    /// @return true if first 'size' bytes constitute a complete header
    bool isComplete(size_t size) const {
        if (size < 2) return false;
        return std::to_integer<uint8_t>(raw[1] & std::byte(0b1111111)) < 126 ? size >= 6 : size >= 14;
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
        for (size_t i = 0; i <= len; ++i) raw[2 + i] = std::byte(size >> (8 * (len - i - 1)));
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
        buffers.emplace_back(raw.data(), headerSize());
        buffers.emplace_back(reinterpret_cast<const std::byte*>(text.data()), text.size());
    }

    Frame(std::span<const std::byte> dataHead, std::span<const std::byte> dataNext = {}) {
        setFIN(true);
        setOpcode(Opcode::binary);
        setPayloadSize(dataHead.size() + dataNext.size());
        buffers.emplace_back(raw.data(), headerSize());
        buffers.emplace_back(dataHead.data(), dataHead.size());
        if (!dataNext.empty()) buffers.emplace_back(dataNext.data(), dataNext.size());
    }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&&) = default; 
    Frame& operator=(Frame&&) = default;

    [[nodiscard]] size_t size() const { return payloadSize(); };

    std::vector<typename Net::ConstBuffer> toBuffers() const { return buffers; }

    /// @return size of added buffer
    size_t addBuffer(std::span<const std::byte> buffer) {
        std::cout << "frame::addBuffer " << utils::hexDump(buffer) << "\n";
        setPayloadSize(payloadSize() + buffer.size());
        buffers.emplace_back(buffer.data(), buffer.size());
        return buffer.size();
    }

    std::vector<typename Net::ConstBuffer> buffers;
};

class FrameDecoder {
public:
    Header::Opcode frameType;

public:
    FrameDecoder() : payloadBuffer(sizeof(Header)) { reset(); }
    std::span<const std::byte> payload() const { return std::span(payloadBuffer.data(), payloadSize); }

    // Parses some incoming data and tries to decode it.
    // @return true if the frame is complete, false if it needs more data
    bool parse(std::span<const std::byte> buffer) {
        auto decodePayload = [&](std::span<const std::byte> encoded) -> size_t {
            for (auto in : encoded) payloadBuffer.push_back(in ^ mask[maskIndex++ % 4]);
            return payloadBuffer.size();
        };
        auto decodeHeader = [&](auto input) {
            payloadSize = reinterpret_cast<const Header*>(input)->payloadSize();
            headerSize = reinterpret_cast<const Header*>(input)->headerSize();
            mask = reinterpret_cast<const Header*>(input)->maskingKey();
            frameType = reinterpret_cast<const Header*>(input)->opcode();
            payloadBuffer.reserve(payloadSize);
        };
        auto bufferizeHeaderData = [&](std::span<const std::byte> input) -> size_t {
            for (size_t index = 0; index < input.size(); ++index) {
                headerBuffer.raw[headerBufferParser++] = *(input.data() + index);
                if (headerBuffer.isComplete(headerBufferParser)) return index + 1;
            }
            return input.size();
        };

        switch (state) {
        case DecodingState::starting:
            if (reinterpret_cast<const Header*>(buffer.data())->isComplete(buffer.size())) {
                reinterpret_cast<const Header*>(buffer.data())->dump();
                decodeHeader(buffer.data());
                if (decodePayload(buffer.subspan(headerSize, std::min(buffer.size() - headerSize, payloadSize))) == payloadSize) return true;
                state = DecodingState::decodingPayload;
            }
            else {
                bufferizeHeaderData(buffer);
                state = DecodingState::partialHeader;
            }
            break;
        case DecodingState::partialHeader: {
            auto consumedData = bufferizeHeaderData(buffer);
            if (headerBuffer.isComplete(headerBufferParser)) {
                decodeHeader(headerBuffer.raw.data());
                if (decodePayload(buffer.subspan(consumedData, std::min(buffer.size() - consumedData, payloadSize))) == payloadSize) return true;
                state = DecodingState::decodingPayload;
            }
        } break;
        case DecodingState::decodingPayload: {
            decodePayload(buffer.first(std::min(payloadSize - payloadBuffer.size(), buffer.size())));
            return (buffer.size() >= (payloadSize - payloadBuffer.size()));
        }
        }
        return false;
    }

    void reset() {
        maskIndex = 0;
        headerBufferParser = 0;
        payloadBuffer.clear();
        state = DecodingState::starting;
    }

private:
    enum class DecodingState { starting, partialHeader, decodingPayload } state;
    std::vector<std::byte> payloadBuffer;
    Header headerBuffer;
    size_t headerBufferParser;
    size_t payloadSize, headerSize;
    std::array<std::byte, 4> mask;
    uint8_t maskIndex;
};

template<typename Net>
class WebSocket {
    typename Net::Socket socket;
    static constexpr size_t receptionBufferSize = 8192;

public:
    explicit WebSocket(typename Net::Socket netSocket) : socket(std::move(netSocket)), started(false) {
        log::debug("WebSocket constructor");
        start();
    }
    WebSocket(const WebSocket&) = delete;
    WebSocket(WebSocket&&) = default;
    WebSocket& operator=(const WebSocket&) = delete;
    WebSocket& operator=(WebSocket&&) = default;
    ~WebSocket() { log::debug("WebSocket destructor"); }

    void start() {
        started = true;
        read();
    }

    void stop() {
        started = false;
        socket.close();
    }

    void onMessage(std::function<void(std::string_view)>&& handler) { textHandler = std::move(handler); }
    void onMessage(std::function<void(std::span<const std::byte>)>&& handler) { binaryHandler = std::move(handler); }
    void onClose(std::function<void(CloseEvent)>&& handler) { closeHandler = std::move(handler); }
    void write(std::string_view text) { writeData(Frame<Net>(text)); }
    void write(std::span<const std::byte> data) { writeData(Frame<Net>(data)); }
    void write(std::span<const std::byte> data, std::span<const std::byte> data2) { writeData(Frame<Net>(data, data2)); }
    void write(Frame<Net> frame) { writeData(std::move(frame)); }

private:
    std::array<std::byte, receptionBufferSize> readBuffer;
    FrameDecoder decoder;
    std::function<void(std::string_view)> textHandler;
    std::function<void(std::span<const std::byte>)> binaryHandler;
    std::function<void(CloseEvent)> closeHandler;
    bool started;

private:
    void read() {
        socket.async_read_some(Net::Buffer(readBuffer), [this](std::error_code ec, std::size_t bytesTransferred) {
            if (!ec) {
                if (decoder.parse(std::span(readBuffer.data(), bytesTransferred))) {
                    auto data = decoder.payload();
                    switch (decoder.frameType) {
                    case Header::Opcode::text:
                        if (textHandler) textHandler(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
                        break;
                    case Header::Opcode::binary:
                        if (binaryHandler) binaryHandler(data);
                        break;
                    case Header::Opcode::connectionClose:
                        if (closeHandler) closeHandler(CloseEvent{});
                        stop();
                        break;
                    default: std::cout << "Unhandled frameType";
                    };
                    decoder.reset();
                }
                read();
            }
            else {
                log::error("Error in websocket::read() : {}:{}", ec.value(), ec.message());
                if (closeHandler) closeHandler(CloseEvent{static_cast<uint16_t>(ec.value()), ec.message()});
                stop();
            }
        });
    }

    void writeData(Frame<Net> frame) {
        Net::AsyncWrite(socket, frame.toBuffers(), [this](std::error_code ec, std::size_t /*bytesTransferred*/) {
            if (ec) {
                if (started) {
                    log::error("Error during write : ec.value() = {}", ec.value());
                    if (closeHandler) closeHandler(CloseEvent{static_cast<uint16_t>(ec.value()), ec.message()});
                    if (ec != Net::Error::OperationAborted) stop();
                }
            }
        });
    }
};

} // namespace webfront::websocket
