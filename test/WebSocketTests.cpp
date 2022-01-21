#include <WebSocket.hpp>

#include <catch2/catch.hpp>

#include <array>

using namespace webfront;
using namespace std;

SCENARIO("WebSocket Headers decoding") {
	GIVEN("Some Header data") {
		array<uint8_t, 14> data{ 0x1, 0b10000000 | 63, 0x10, 0x11, 0x12, 0x13 };
		WHEN("Casting to header") {
			auto header = reinterpret_cast<const websocket::Header*>(data.data());
			THEN("Fields should be") {
				REQUIRE(header->FIN() == false);
				REQUIRE(header->RSV1() == false);
				REQUIRE(header->RSV2() == false);
				REQUIRE(header->RSV3() == false);
				REQUIRE(header->opcode() == websocket::Header::Opcode::text);
				REQUIRE(header->MASK() == true);
				REQUIRE(header->maskingKey() == std::array<std::byte, 4>{std::byte{ 0x10 }, std::byte{ 0x11 }, std::byte{ 0x12 }, std::byte{ 0x13 }});
				REQUIRE(header->payloadLen() == 63);
				REQUIRE(header->getPayloadSize() == 63);
				REQUIRE(header->headerSize() == 6);
			}
		}
	}

	GIVEN("Header data") {
		array<uint8_t, 14> data{ 0x1, 0b10000000 | 126, 10, 20, 30, 40, 50, 60, 70, 80, 0x10, 0x11, 0x12, 0x13 };
		WHEN("Casting to header") {
			auto header = reinterpret_cast<const websocket::Header*>(data.data());
			THEN("Payload size should be 31127 from the 16 bits field") {
				REQUIRE(header->getPayloadSize() == 2580);
				REQUIRE(header->headerSize() == 14);
			}
		}
	}

	GIVEN("Header data") {
		array<uint8_t, 14> data{ 0x1, 0b10000000 | 127, 10, 20, 30, 40, 50, 60, 70, 80, 0x10, 0x11, 0x12, 0x13 };
		WHEN("Casting to header") {
			auto header = reinterpret_cast<const websocket::Header*>(data.data());
			THEN("Payload size should be 13 256 444 from the 64 bits field") {
				REQUIRE(header->getPayloadSize() == 726238597903828560);
				REQUIRE(header->headerSize() == 14);
			}
		}
	}
}

SCENARIO("WebSocket decoder") {
	GIVEN("Some frame data and a decoder") {
		array<uint8_t, 22> frame{ 0x1, 0b10000000 | 8, 0x10, 0x11, 0x12, 0x13,
			uint8_t{'H' ^ 0x10}, uint8_t{'e' ^ 0x11}, uint8_t{'l' ^ 0x12}, uint8_t{'l' ^ 0x13}, uint8_t{'o' ^ 0x10}, uint8_t{' ' ^ 0x11}, uint8_t{'W' ^ 0x12}, uint8_t{'S' ^ 0x13} };
		websocket::FrameDecoder decoder;
		WHEN("All data is received in one unique chunk") {
			REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), frame.size())));
			auto bufferParser = std::cbegin(decoder.getPayload());
			THEN("Payload is correctly decoded") {
				for (auto c : { 'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S' })
					REQUIRE(std::to_integer<uint8_t>(*bufferParser++) == c);
			}
		}

		WHEN("Data is received in two chunks but the first has a complete header") {
			REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), 7)) == false);
			REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data() + 7), frame.size() - 7)));
			auto bufferParser = std::cbegin(decoder.getPayload());
			THEN("Payload is correctly decoded") {
				for (auto c : { 'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S' })
					REQUIRE(std::to_integer<uint8_t>(*bufferParser++) == c);
			}
		}

		WHEN("Data is received in two chunks but the first has an incomplete header") {
			REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data()), 3)) == false);
			REQUIRE(decoder.parse(std::span(reinterpret_cast<const std::byte*>(frame.data() + 3), frame.size() - 3)));
			auto bufferParser = std::cbegin(decoder.getPayload());
			THEN("Payload is correctly decoded") {
				for (auto c : { 'H', 'e', 'l', 'l', 'o', ' ', 'W', 'S' })
					REQUIRE(std::to_integer<uint8_t>(*bufferParser++) == c);
			}
		}
	}
}