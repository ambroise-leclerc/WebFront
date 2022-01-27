/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief WebSocket protocol implementation - RFC6455
#pragma once
#include "details/HexDump.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <span>

#include <iostream>

namespace webfront {

template<typename ConnectionType>
class Connections {
public:
    Connections(const Connections&) = delete;
    Connections& operator=(const Connections&) = delete;
    Connections() = default;

    void start(std::shared_ptr<ConnectionType> connection) {
        connections.insert(connection);
        connection->start();
    }

    void stop(std::shared_ptr<ConnectionType> connection) {
        connections.erase(connection);
        connection->stop();
    }

    void stopAll() {
        for (auto connection : connections) connection->stop();
        connections.clear();
    }

private:
    std::set<std::shared_ptr<ConnectionType>> connections;
};

namespace websocket {
using Handle = uint32_t;

struct Header {
    std::array<std::byte, 14> raw;
    Header() : raw{} {}

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
        ctrl5
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
        using namespace std;
        cout << "FIN  : " << FIN() << "\n";
        cout << "RSV1 : " << RSV1() << "\n";
        cout << "RSV2 : " << RSV2() << "\n";
        cout << "RSV3 : " << RSV3() << "\n";
        cout << "opcode : " << static_cast<int>(opcode()) << "\n";
        cout << "MASK : " << MASK() << "\n";
        cout << "payloadLenField : " << +payloadLenField() << "\n";
        cout << "extendedLenField : " << extendedLenField() << "\n";
        auto k = maskingKey();
        cout << "maskingKey : 0x" << hex << to_integer<int>(k[0]) << to_integer<int>(k[1]) << to_integer<int>(k[2]) << to_integer<int>(k[3]) << dec << "\n";
        cout << "getPayloadSize : " << payloadSize() << "\n";
        cout << "headerSize : " << headerSize() << "\n";
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
        raw[1] &= std::byte(0b10000000);
        auto f = [&size, this ](size_t i, uint8_t shift = 0) constexpr { return raw[i] = static_cast<std::byte>(size >> shift); };
        if (size < 126)
            raw[1] |= static_cast<std::byte>(size);
        else if (size < 65536) {
            raw[1] |= std::byte(126);
            f(2, 8);
            f(3);
        }
        else {
            raw[1] |= std::byte(127);
            f(2, 56);
            f(3, 48);
            f(4, 40);
            f(5, 32);
            f(6, 24);
            f(7, 16);
            f(8, 8);
            f(9);
        }
    }

protected:
    bool test(size_t index, uint8_t bit) const { return std::to_integer<bool>(raw[index] & std::byte(1 << bit)); }
};

struct CloseEvent {
    uint16_t status;
    std::string reason;
};

struct Frame : public Header {
    Frame(std::string_view text) {
        setFIN(true);
        setOpcode(Opcode::text);
        setPayloadSize(text.size());
        dataSpan = std::span(reinterpret_cast<const std::byte*>(text.data()), text.size());
    }
    size_t size() const { return payloadSize(); };
    const std::byte* data() const { return reinterpret_cast<const std::byte*>(this + headerSize()); }

    template<typename Net>
    std::vector<typename Net::ConstBuffer> toBuffers() const {
        std::vector<typename Net::ConstBuffer> buffers;
        buffers.push_back(typename Net::ConstBuffer(raw.data(), headerSize()));
        buffers.push_back(typename Net::ConstBuffer(dataSpan.data(), dataSpan.size()));
        return buffers;
    }

    std::span<const std::byte> dataSpan;
};

class FrameDecoder {
public:
    Header::Opcode frameType;

public:
    FrameDecoder() : payloadBuffer(sizeof(Header)) { reset(); }
    std::span<const std::byte> payload() const { return std::span(payloadBuffer.cbegin(), payloadSize); }

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
class WebSocket : public std::enable_shared_from_this<WebSocket<Net>> {
    typename Net::Socket socket;
    Connections<WebSocket>& webSockets;

public:
    explicit WebSocket(typename Net::Socket netSocket, Connections<WebSocket>& connections) : socket(std::move(netSocket)), webSockets(connections) {}
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    void start() { read(); }
    void stop() { socket.close(); }

    void onMessage(std::function<void(std::string_view)> handler) { textHandler = std::move(handler); }
    void onMessage(std::function<void(std::span<const std::byte>)> handler) { binaryHandler = std::move(handler); }
    void onClose(std::function<void(CloseEvent)> handler) { closeHandler = std::move(handler); }
    void write(std::string_view text) {
        Frame frame(text);
        auto self(this->shared_from_this());
        std::error_code ec;
        Net::Write(socket, frame.toBuffers<Net>(), ec);
        if (ec) {
            std::clog << "Error during write : ec.value() = " << ec.value() << "\n";
            webSockets.stop(self);
        }
    }

private:
    std::array<std::byte, 8192> readBuffer;
    FrameDecoder decoder;
    std::function<void(std::string_view)> textHandler;
    std::function<void(std::span<const std::byte>)> binaryHandler;
    std::function<void(CloseEvent)> closeHandler;

private:
    void read() {
        auto self(this->shared_from_this());
        socket.async_read_some(Net::Buffer(readBuffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
            if (!ec) {
                std::cout << "Received " << bytesTransferred << " bytes\n" << utils::HexDump(std::span(readBuffer.data(), bytesTransferred)) << "\n";
                if (decoder.parse(std::span(readBuffer.data(), bytesTransferred))) {
                    auto data = decoder.payload();
                    switch (decoder.frameType) {
                    case Header::Opcode::text:
                        if (textHandler) textHandler(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
                        break;
                    case Header::Opcode::binary:
                        if (binaryHandler) binaryHandler(data);
                        break;
                    case Header::Opcode::connectionClose: webSockets.stop(self); break;
                    default: std::cout << "Unhandled frameType";
                    };
                    decoder.reset();
                }
                read();
            }
            else {
                std::clog << "Error in websocket::read() : " << ec << "\n";
                webSockets.stop(self);
            }
        });
    }
};

struct WSManagerConfigurationError : std::runtime_error {
    WSManagerConfigurationError() : std::runtime_error("WSManager handler configuration error") {}
};

template<typename Net>
class WSManager {
    Connections<WebSocket<Net>> webSockets;
    std::function<void(std::shared_ptr<WebSocket<Net>>)> openHandler;

public:
    void onOpen(std::function<void(std::shared_ptr<WebSocket<Net>>)> handler) { openHandler = std::move(handler); }

public:
    void createWebSocket(typename Net::Socket socket) {
        if (!openHandler) throw WSManagerConfigurationError();

        auto ws = std::make_shared<WebSocket<Net>>(std::move(socket), webSockets);
        openHandler(ws);
        webSockets.start(ws);
    };
};

} // namespace websocket
} // namespace webfront