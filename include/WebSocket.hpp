#pragma once
#include <array>
#include <experimental/net>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace webfront {
namespace websocket {
namespace net = std::experimental::net;
using Handle = uint32_t;

struct Header {
	uint8_t raw[14];

	enum class Opcode {
		continuation, text, binary, reserved1, reserved2, reserved3, reserved4, reserved5,
		connectionClose, ping1, ping2, ctrl1, ctrl2, ctrl3, ctrl4, ctrl5
	};

	bool FIN() const { return (raw[0] & 0b10000000) != 0; }
	bool RSV1() const { return (raw[0] & 0b01000000) != 0; }
	bool RSV2() const { return (raw[0] & 0b00100000) != 0; }
	bool RSV3() const { return (raw[0] & 0b00010000) != 0; }
	Opcode opcode() const { return static_cast<Opcode>(raw[0] & 0b1111); }
	bool MASK() const { return (raw[1] & 0b10000000) != 0; }
	uint8_t payloadLen() const { return raw[1] & 0b1111111; }
	uint64_t extendedLen() const { return (uint64_t(raw[2]) << 56 ) | (uint64_t(raw[3]) << 48) | (uint64_t(raw[4]) << 40) | (uint64_t(raw[5]) << 32) | (uint64_t(raw[6]) << 24) | (uint64_t(raw[7]) << 16) | (uint64_t(raw[8]) << 8) | raw[9]; }
	std::array<std::byte, 4> maskingKey() const { return { std::byte{raw[10]}, std::byte{raw[11]}, std::byte{raw[12]}, std::byte{raw[13]} }; }

	uint64_t getPayloadSize() const { 
		auto len = payloadLen();
		return len < 126 ? len : extendedLen() >> (len == 126 ? 48 : 0); }
};

struct FramePayload : public Header {
	size_t size() const { return static_cast<size_t>(getPayloadSize()); };
	const std::byte* data() const { return reinterpret_cast<const std::byte*>(this + sizeof(Header)); }
};
static_assert(sizeof(Header) == 14, "WebSocket Header size must be 14 bytes");
static_assert(sizeof(FramePayload) == 14, "WebSocket Frame size must be 14 bytes");

class FrameDecoder {
public:
	FrameDecoder() : frameBuffer(sizeof(Header)) {
		reset();
	}

	// Parses some incoming data and tries to decode it.
	// @return true if the frame is complete, false it it needs more data
	bool parse(std::span<const std::byte> buffer) {
		auto decodePayload = [&](std::span<const std::byte> encoded) -> size_t {
			for (auto in : encoded)
				*frameBufferItor++ = in ^ mask[maskIndex++ % 4];
			return frameBufferItor - frameBuffer.cbegin() - sizeof(Header);
		};
		auto copyInFrameBuffer = [&](size_t size) {
			std::copy(buffer.data(), buffer.data() + size, frameBufferItor);
			frameBufferItor += size;
		};
		auto decodeHeader = [&](auto input) {
			totalSize = reinterpret_cast<const Header*>(input)->getPayloadSize() + sizeof(Header);
			mask = reinterpret_cast<const Header*>(input)->maskingKey();
			if (totalSize > frameBuffer.size()) {
				frameBuffer.resize(totalSize);
			}
			frameBufferItor = frameBuffer.begin() + sizeof(Header);
		};

		switch (state) {
			case DecodingState::starting:
				if (buffer.size() >= sizeof(Header)) {
					decodeHeader(buffer.data());
					if (decodePayload(buffer.subspan(sizeof(Header), std::min(buffer.size(), totalSize) - sizeof(Header))) == (totalSize - sizeof(Header)))
						return true;
					state = DecodingState::decodingPayload;
				}
				else {
					copyInFrameBuffer(buffer.size());
					state = DecodingState::partialHeader;
				}
				break;
			case DecodingState::partialHeader: {
				auto bytesToCompleteHeader = sizeof(Header) - (frameBufferItor - frameBuffer.cbegin());
				copyInFrameBuffer(std::min(bytesToCompleteHeader, buffer.size()));
				if (buffer.size() >= bytesToCompleteHeader) {
					decodeHeader(frameBuffer.data());
					if (decodePayload(buffer.subspan(bytesToCompleteHeader, buffer.size() - bytesToCompleteHeader)) == (totalSize - sizeof(Header)))
						return true;
					state = DecodingState::decodingPayload;
				}
			} break;
			case DecodingState::decodingPayload: {
				auto decodedSize = frameBufferItor - frameBuffer.cbegin();
				decodePayload(buffer.last(std::min(totalSize - decodedSize, buffer.size())));
				if (buffer.size() >= (totalSize - decodedSize))
					return true;
			} break;
		}
		return false;
	}

	std::span<const std::byte> getPayload() {
		return std::span(frameBuffer.cbegin() + sizeof(Header), totalSize - sizeof(Header));
	}

	void reset() {
		frameBufferItor = frameBuffer.begin();
		maskIndex = 0;
		state = DecodingState::starting;
	}

private:
	enum class DecodingState { starting, partialHeader, decodingPayload } state;
	std::vector<std::byte> frameBuffer;
	std::vector<std::byte>::iterator frameBufferItor;
	size_t totalSize;
	std::array<std::byte, 4> mask;
	uint8_t maskIndex;
};

class WebSocket : public std::enable_shared_from_this<WebSocket> {
	net::ip::tcp::socket socket;
	Handle handle;

public:
	std::function<void()> onConnected;

public:
	WebSocket(net::ip::tcp::socket socket, Handle handle) : socket(std::move(socket)), handle(handle) {
		read();
	}

private:
	std::array<std::byte, 8192> readBuffer;
	FrameDecoder decoder;

private:
	void read() {
		auto self(shared_from_this());
		socket.async_read_some(net::buffer(readBuffer), [this, self](std::error_code /*ec*/, std::size_t bytesTransferred) {
			if (decoder.parse(std::span(readBuffer.data(), bytesTransferred))) {
				decoder.reset();
			}
			read();
		});
	}
};


class WSManager {
	std::vector<std::shared_ptr<WebSocket>> webSockets;
	Handle handlesCounter = 0;
public:
	std::function<void(std::shared_ptr<WebSocket>)> onConnected;

public:
	void createWebSocket(net::ip::tcp::socket socket) {
		auto ws = std::make_shared<WebSocket>(std::move(socket), handlesCounter);
		webSockets.push_back(ws);
		handlesCounter = static_cast<Handle>(webSockets.size());
		if (onConnected) onConnected(ws);
	};
};


} // namespace websocket
} // namespace webfront