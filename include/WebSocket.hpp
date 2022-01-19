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

#pragma pack(push, 1)
struct Header {
	enum class Opcode {
		continuation, text, binary, reserved1, reserved2, reserved3, reserved4, reserved5,
		connectionClose, ping1, ping2, ctrl1, ctrl2, ctrl3, ctrl4, ctrl5
	};
	/*uint16_t FIN : 1;
	uint16_t RSV1 : 1;
	uint16_t RSV2 : 1;
	uint16_t RSV3 : 1;
	uint16_t opcode : 4;
	uint16_t MASK : 1;
	uint16_t payloadLen : 7;

	uint64_t extendedLen : 64;
	uint32_t maskingKey;
	*/

	uint8_t raw[14];
	bool FIN() const { return (raw[0] & 0b10000000) != 0; }
	bool RSV1() const { return (raw[0] & 0b01000000) != 0; }
	bool RSV2() const { return (raw[0] & 0b00100000) != 0; }
	bool RSV3() const { return (raw[0] & 0b00010000) != 0; }
	Opcode opcode() const { return static_cast<Opcode>(raw[0] & 0b1111); }
	bool MASK() const { return (raw[1] & 0b10000000) != 0; }
	uint8_t payloadLen() const { return raw[1] & 0b1111111; }
	uint64_t extendedLen() const { return (uint64_t(raw[2]) << 56 ) | (uint64_t(raw[3]) << 48) | (uint64_t(raw[4]) << 40) | (uint64_t(raw[5]) << 32) | (uint64_t(raw[6]) << 24) | (uint64_t(raw[7]) << 16) | (uint64_t(raw[8]) << 8) | raw[9]; }
	uint32_t maskingKey() const { return (uint32_t(raw[10]) << 24) | (uint32_t(raw[11]) << 16) | (uint32_t(raw[12]) << 8) | raw[13]; }

	
	uint64_t getPayloadSize() const { 
		auto len = payloadLen();
		return len < 126 ? len : extendedLen() >> (len == 126 ? 48 : 0); }
};

struct FramePayload : public Header {
	size_t size() const { return static_cast<size_t>(getPayloadSize()); };
	const std::byte* data() const { return reinterpret_cast<const std::byte*>(this + sizeof(Header)); }
};
#pragma pack(pop)
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
		auto copyInFrameBuffer = [&](size_t size) {
			std::copy(buffer.data(), buffer.data() + size, frameBufferItor);
			frameBufferItor += size;
		};
		auto startChunksDecoding = [&](DecodingState nextState) {
			payload = reinterpret_cast<const FramePayload*>(frameBuffer.data());
			copyInFrameBuffer(buffer.size());
			state = nextState;
		};

		if (state == DecodingState::starting) {
			if (buffer.size() >= sizeof(Header)) {
				auto payloadSize = reinterpret_cast<const Header*>(buffer.data())->getPayloadSize();
				if (buffer.size() >= payloadSize) {
					payload = reinterpret_cast<const FramePayload*>(buffer.data());
					return true;
				}
				else {
					if (frameBuffer.size() < (payloadSize + sizeof(Header)))
						frameBuffer.resize(payloadSize + sizeof(Header));
					startChunksDecoding(DecodingState::chunksReceived);
				}
			}
			else startChunksDecoding(DecodingState::partialHeader);
		}
		else if (state == DecodingState::partialHeader) {
			auto neededHeaderBytes = sizeof(Header) - (frameBufferItor - frameBuffer.begin());
			if (buffer.size() < neededHeaderBytes) copyInFrameBuffer(buffer.size());
			else {
				copyInFrameBuffer(neededHeaderBytes);
				auto totalSize = payload->size() + sizeof(Header);
				if (frameBuffer.size() < totalSize)
					frameBuffer.resize(totalSize);
				std::copy(buffer.data() + neededHeaderBytes, buffer.data() + buffer.size(), frameBufferItor);
				frameBufferItor += buffer.size() - neededHeaderBytes;

				if (size_t(frameBufferItor - frameBuffer.begin()) >= totalSize)
					return true;
				state = DecodingState::chunksReceived;
			}
		}
		else if (state == DecodingState::chunksReceived) {
			copyInFrameBuffer(buffer.size());
			if (size_t(frameBufferItor - frameBuffer.begin()) >= payload->size() + sizeof(Header))
				return true;
		}
		return false;
	}

	std::span<const std::byte> getPayload() {
		return std::span(payload->data(), payload->size());
	}

	void getPayload(auto buffer) const {
		std::copy(payload->data(), payload->data() + std::min(buffer.size(), payload->size()), buffer.data());
	}

	void reset() {
		frameBufferItor = frameBuffer.begin();
		state = DecodingState::starting;
	}

private:
	enum class DecodingState { starting, partialHeader, chunksReceived } state;
	const FramePayload* payload;
	std::vector<std::byte> frameBuffer;
	std::vector<std::byte>::iterator frameBufferItor;
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