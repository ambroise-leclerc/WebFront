#pragma once

#include "details/HexDump.hpp"

#include <array>
#include <experimental/net>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <iostream>

namespace webfront {
namespace websocket {
namespace net = std::experimental::net;
using Handle = uint32_t;

struct Header {
	std::array<uint8_t, 14> raw;

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
	
	size_t headerSize() const {
		if (payloadLen() < 126) return MASK() ? 6 : 2;
		return MASK() ? 14 : 10;
	}

	std::array<std::byte, 4> maskingKey() const {
		if (!MASK()) return {};
		auto index = headerSize() - 4;
		return { std::byte{raw[index]}, std::byte{raw[index + 1] }, std::byte{ raw[index + 2] }, std::byte{ raw[index + 3] } };
	}

	uint64_t getPayloadSize() const { 
		auto len = payloadLen();
		return len < 126 ? len : extendedLen() >> (len == 126 ? 48 : 0); 
	}
	uint64_t getFrameSize() const { return getPayloadSize() + headerSize(); }

	void dump() const {
		using namespace std;
		cout << "FIN  : " << FIN() << "\n";
		cout << "RSV1 : " << RSV1() << "\n";
		cout << "RSV2 : " << RSV2() << "\n";
		cout << "RSV3 : " << RSV3() << "\n";
		cout << "opcode : " << static_cast<int>(opcode()) << "\n";
		cout << "MASK : " << MASK() << "\n";
		cout << "payloadLen : " << +payloadLen() << "\n";
		cout << "extendedLen : " << extendedLen() << "\n";
		auto k = maskingKey();
		cout << "maskingKey : 0x" << hex << to_integer<int>(k[0]) << to_integer<int>(k[1]) << to_integer<int>(k[2]) << to_integer<int>(k[3]) << dec << "\n";
		cout << "getPayloadSize : " << getPayloadSize() << "\n";
		cout << "headerSize : " << headerSize() << "\n";
	}
	
	/// @return true if given data starts with a complete header
	static bool dataIsComplete(auto data) {
		if (data.size() < 2) return false;
		return (std::to_integer<uint8_t>(data[1]) & 0b1111111) < 126 ? data.size() >= 6 : data.size() >= 14;
	}

	bool isComplete(size_t size) const {
		if (size < 2) return false;
		return (raw[1] & 0b1111111) < 126 ? size >= 6 : size >= 14;
	}
};

struct FramePayload : public Header {
	size_t size() const { return static_cast<size_t>(getPayloadSize()); };
	const std::byte* data() const { return reinterpret_cast<const std::byte*>(this + headerSize()); }
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
			return frameBufferItor - frameBuffer.cbegin() - headerSize;
		};
		auto decodeHeader = [&](auto input) {
			auto header = reinterpret_cast<const Header*>(input);
			frameSize = header->getFrameSize();
			headerSize = header->headerSize();
			header->dump();
			mask = header->maskingKey();
			if (frameSize > frameBuffer.size()) {
				frameBuffer.resize(frameSize);
			}
			frameBufferItor = frameBuffer.begin() + headerSize;
			std::cout << "WSFrame totalSize : " << frameSize << "\n";
		};

		auto storeBufferData = [&]() {
			for (size_t index = 0; index < buffer.size(); ++index) {
				headerBuffer.raw[headerBufferParser++] = std::to_integer<uint8_t>(*(buffer.data() + index));
				if (headerBuffer.isComplete(headerBufferParser))
					return index + 1;
			}
			return buffer.size();
		};

		switch (state) {
			case DecodingState::starting:
				if (Header::dataIsComplete(buffer)) {
					decodeHeader(buffer.data());
					if (decodePayload(buffer.subspan(headerSize, std::min(buffer.size(), frameSize) - headerSize)) == (frameSize - headerSize))
						return true;
					state = DecodingState::decodingPayload;
				}
				else {
					storeBufferData();
					state = DecodingState::partialHeader;
				}
				break;
			case DecodingState::partialHeader: {
				auto consumedData = storeBufferData();
				if (headerBuffer.isComplete(headerBufferParser)) {
					decodeHeader(headerBuffer.raw.data());
					if (decodePayload(buffer.subspan(consumedData, std::min(buffer.size() - consumedData, frameSize - headerSize))) == (frameSize - headerSize))
						return true;
					state = DecodingState::decodingPayload;
				}
			} break;
			case DecodingState::decodingPayload: {
				auto decodedSize = frameBufferItor - frameBuffer.cbegin();
				decodePayload(buffer.first(std::min(frameSize - decodedSize, buffer.size())));
				if (buffer.size() >= (frameSize - decodedSize))
					return true;
			} break;
		}
		return false;
	}

	std::span<const std::byte> getPayload() {
		return std::span(frameBuffer.cbegin() + headerSize, frameSize - headerSize);
	}

	void reset() {
		frameBufferItor = frameBuffer.begin();
		maskIndex = 0;
		headerBufferParser = 0;
		state = DecodingState::starting;
	}

private:
	enum class DecodingState { starting, partialHeader, decodingPayload } state;
	std::vector<std::byte> frameBuffer;
	std::vector<std::byte>::iterator frameBufferItor;
	Header headerBuffer;
	size_t headerBufferParser;
	size_t frameSize, headerSize;
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
	}

	void start() { read(); }

private:
	std::array<std::byte, 8192> readBuffer;
	FrameDecoder decoder;

private:
	void read() {
		std::cout << "websocket read()\n";
		auto self(shared_from_this());
		socket.async_read_some(net::buffer(readBuffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
			if (!ec) {
				std::cout << "Received " << bytesTransferred << " bytes\n" << utils::HexDump(std::span(readBuffer.data(), bytesTransferred)) << "\n";
				if (decoder.parse(std::span(readBuffer.data(), bytesTransferred))) {
					auto payload = decoder.getPayload();
					std::cout << "Decoded into :\n" << utils::HexDump(payload) << "\n";
					decoder.reset();
				}
				read();
			}
			else std::cout << "Error in websocket::read() : " << ec << "\n";
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