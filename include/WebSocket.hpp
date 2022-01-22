/// @file WebSocket.hpp
/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief WebSocket protocol implementation - RFC6455
#pragma once

#include "details/HexDump.hpp"

#include <array>
#include <experimental/net>
#include <functional>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include <iostream>

namespace webfront {
namespace websocket {
namespace net = std::experimental::net;
using Handle = uint32_t;

struct Header {
	std::array<uint8_t, 14> raw;
	Header() : raw{} {}

	enum class Opcode {
		continuation, text, binary, reserved1, reserved2, reserved3, reserved4, reserved5,
		connectionClose, ping, pong, ctrl1, ctrl2, ctrl3, ctrl4, ctrl5
	};

	bool FIN() const { return (raw[0] & 0b10000000) != 0; }
	bool RSV1() const { return (raw[0] & 0b01000000) != 0; }
	bool RSV2() const { return (raw[0] & 0b00100000) != 0; }
	bool RSV3() const { return (raw[0] & 0b00010000) != 0; }
	Opcode opcode() const { return static_cast<Opcode>(raw[0] & 0b1111); }
	bool MASK() const { return (raw[1] & 0b10000000) != 0; }
	uint8_t payloadLenField() const { return raw[1] & 0b1111111; }
	uint64_t extendedLenField() const {
		return payloadLenField() == 126 ? (uint16_t(raw[2] << 8) | uint16_t(raw[3]))
			: (uint64_t(raw[2]) << 56) | (uint64_t(raw[3]) << 48) | (uint64_t(raw[4]) << 40) | (uint64_t(raw[5]) << 32) | (uint64_t(raw[6]) << 24) | (uint64_t(raw[7]) << 16) | (uint64_t(raw[8]) << 8) | raw[9];
	}
	
	size_t headerSize() const {
		if (payloadLenField() < 126) return MASK() ? 6 : 2;
		if (payloadLenField() == 126) return MASK() ? 8 : 4;
		return MASK() ? 14 : 10;
	}

	std::array<std::byte, 4> maskingKey() const {
		if (!MASK()) return {};
		auto index = headerSize() - 4;
		return { std::byte{raw[index]}, std::byte{raw[index + 1] }, std::byte{ raw[index + 2] }, std::byte{ raw[index + 3] } };
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
		return (raw[1] & 0b1111111) < 126 ? size >= 6 : size >= 14;
	}

	void setFIN(bool set) { if (set) raw[0] |= 1 << 7; else raw[0] &= 0b1111111; }
	void setOpcode(Opcode code) { raw[0] &= 0b11110000; raw[0] |= static_cast<uint8_t>(code); }
	void setPayloadSize(size_t size) {
		raw[1] &= 0b10000000;
		if (size < 126)
			raw[1] |= size;
		else if (size < 65536) {
			raw[1] |= 126; raw[2] = uint8_t(size >> 8); raw[3] = uint8_t(size & 0xFF);
		}
		else {
			raw[1] |= 127; raw[2] = size >> 56; raw[3] = (size >> 48) & 0xFF; raw[4] = (size >> 40) & 0xFF; raw[5] = (size >> 32) & 0xFF;
			raw[6] = (size >> 24) & 0xFF; raw[7] = (size >> 16) & 0xFF; raw[8] = (size >> 8) & 0xFF; raw[9] = size & 0xFF;
		}
	}
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
	size_t size() const { return static_cast<size_t>(payloadSize()); };
	const std::byte* data() const { return reinterpret_cast<const std::byte*>(this + headerSize()); }

	std::vector<net::const_buffer> toBuffers() const {
		std::vector<net::const_buffer> buffers;
		buffers.push_back(net::const_buffer(raw.data(), headerSize()));
		buffers.push_back(net::const_buffer(dataSpan.data(), dataSpan.size()));
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
	// @return true if the frame is complete, false it it needs more data
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
				headerBuffer.raw[headerBufferParser++] = std::to_integer<uint8_t>(*(input.data() + index));
				if (headerBuffer.isComplete(headerBufferParser))
					return index + 1;
			}
			return input.size();
		};

		switch (state) {
			case DecodingState::starting:
				if (reinterpret_cast<const Header*>(buffer.data())->isComplete(buffer.size())) {
					decodeHeader(buffer.data());
					if (decodePayload(buffer.subspan(headerSize, std::min(buffer.size() - headerSize, payloadSize))) == payloadSize)
						return true;
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
					if (decodePayload(buffer.subspan(consumedData, std::min(buffer.size() - consumedData, payloadSize))) == payloadSize)
						return true;
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

class WebSocket : public std::enable_shared_from_this<WebSocket> {
	net::ip::tcp::socket socket;

public:
	WebSocket(net::ip::tcp::socket socket) : socket(std::move(socket)) {}

	void start() { read(); }

	void onMessage(std::function<void(std::string_view)> handler) { textHandler = std::move(handler); }
	void onMessage(std::function<void(std::span<const std::byte>)> handler) { binaryHandler = std::move(handler); }
	void onClose(std::function<void(CloseEvent)> handler) { closeHandler = std::move(handler); }
	void write(std::string_view text) {
		Frame frame(text);
		auto self(shared_from_this());
		std::error_code ec;
		net::write(socket, frame.toBuffers(), ec);
		if (ec) std::cout << "Error during write : ec.value() = " << ec.value() << "\n";
	}

private:
	std::array<std::byte, 8192> readBuffer;
	FrameDecoder decoder;
	std::function<void(std::string_view)> textHandler;
	std::function<void(std::span<const std::byte>)> binaryHandler;
	std::function<void(CloseEvent)> closeHandler;

private:
	void read() {
		auto self(shared_from_this());
		socket.async_read_some(net::buffer(readBuffer), [this, self](std::error_code ec, std::size_t bytesTransferred) {
			if (!ec) {
				std::cout << "Received " << bytesTransferred << " bytes\n" << utils::HexDump(std::span(readBuffer.data(), bytesTransferred)) << "\n";
				if (decoder.parse(std::span(readBuffer.data(), bytesTransferred))) {
					auto data = decoder.payload();
					switch (decoder.frameType) {
						case Header::Opcode::text: if (textHandler) textHandler(std::string_view(reinterpret_cast<const char*>(data.data()), data.size())); break;
						case Header::Opcode::binary: if (binaryHandler) binaryHandler(data); break;
						default: std::cout << "Unhandled frameType";
					};
					decoder.reset();
				}
				read();
			}
			else std::cout << "Error in websocket::read() : " << ec << "\n";
		});
	}
};


struct WSManagerConfigurationError : std::runtime_error {
	WSManagerConfigurationError() : std::runtime_error("WSManager handler configuration error") {}
};

class WSManager {
	std::vector<std::shared_ptr<WebSocket>> webSockets;
	std::function<void(std::shared_ptr<WebSocket>)> openHandler;
public:
	void onOpen(std::function<void(std::shared_ptr<WebSocket>)> handler) { openHandler = std::move(handler); }

public:
	void createWebSocket(net::ip::tcp::socket socket) {
		if (!openHandler) throw WSManagerConfigurationError();

		auto ws = std::make_shared<WebSocket>(std::move(socket));
		webSockets.push_back(ws);
		openHandler(ws);
		ws->start();
	};
};


} // namespace websocket
} // namespace webfront