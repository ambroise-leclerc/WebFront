#include <catch2/catch_test_macros.hpp>
#include <http/WebSocket.hpp>
#include <networking/NetworkingMock.hpp>
#include <tooling/HexDump.hpp>
#include <weblink/Messages.hpp>

#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <stdexcept>
#include <vector>

using namespace std;
using namespace webfront;

SCENARIO("Handshake message") {
    std::array<uint8_t, 2> raw{0x00, 0x00};
    auto                   handshake = msg::Handshake::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));

    REQUIRE(sizeof(*handshake) == 2);
    REQUIRE(handshake->getEndian() == msg::JSEndian::little);
}

SCENARIO("Ack message") {
    msg::Ack ack;

    REQUIRE(sizeof(ack) == 2);
}

SCENARIO("FunctionCall") {
    GIVEN("Raw data of a call to 'print' function with a small string as parameter") {
        std::array<uint8_t, 36> raw{0x03, 0x02, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x04, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x04, 0x13, 0x48,
                                    0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x20, 0x32, 0x30, 0x32, 0x32};

        auto functionCall = msg::FunctionCall<>::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
        REQUIRE(functionCall->getParametersCount() == 2);
        REQUIRE(functionCall->getPayloadSize() == 28);
        auto [name, undecodedData] = functionCall->getFunctionName();
        REQUIRE(name == "print");
        std::string text;
        functionCall->decodeParameter(text, undecodedData);
        REQUIRE(text == "Hello World of 2022");
        REQUIRE(undecodedData.size() == 0);
    }
    GIVEN("Raw data of a call to 'cppTest' function with a small string, a string and a number as parameters") {
        std::vector<uint8_t> raw{
            0x03, 0x04, 0x00, 0x00, 0xb1, 0x01, 0x00, 0x00, 0x04, 0x07, 0x63, 0x70, 0x70, 0x54, 0x65, 0x73, 0x74, 0x04, 0x35, 0x54, 0x65, 0x78, 0x74, 0x65,
            0x20, 0x64, 0x65, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e,
            0x67, 0x20, 0x70, 0x6f, 0x75, 0x72, 0x20, 0x63, 0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74,
            0x05, 0x65, 0x01, 0x62, 0x69, 0x67, 0x54, 0x65, 0x78, 0x74, 0x20, 0x3a, 0x20, 0x54, 0x65, 0x78, 0x74, 0x65, 0x20, 0x64, 0x65, 0x20, 0x74, 0x65,
            0x73, 0x74, 0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x70, 0x6f, 0x75, 0x72,
            0x20, 0x63, 0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x54, 0x65, 0x78, 0x74, 0x65, 0x20,
            0x64, 0x65, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e, 0x67,
            0x20, 0x70, 0x6f, 0x75, 0x72, 0x20, 0x63, 0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x20,
            0x2d, 0x20, 0x62, 0x69, 0x67, 0x54, 0x65, 0x78, 0x74, 0x20, 0x3a, 0x20, 0x54, 0x65, 0x78, 0x74, 0x65, 0x20, 0x64, 0x65, 0x20, 0x74, 0x65, 0x73,
            0x74, 0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x70, 0x6f, 0x75, 0x72, 0x20,
            0x63, 0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x54, 0x65, 0x78, 0x74, 0x65, 0x20, 0x64,
            0x65, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20,
            0x70, 0x6f, 0x75, 0x72, 0x20, 0x63, 0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x20, 0x2d,
            0x20, 0x62, 0x69, 0x67, 0x54, 0x65, 0x78, 0x74, 0x20, 0x3a, 0x20, 0x54, 0x65, 0x78, 0x74, 0x65, 0x20, 0x64, 0x65, 0x20, 0x74, 0x65, 0x73, 0x74,
            0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x70, 0x6f, 0x75, 0x72, 0x20, 0x63,
            0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x54, 0x65, 0x78, 0x74, 0x65, 0x20, 0x64, 0x65,
            0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x75, 0x66, 0x66, 0x69, 0x73, 0x61, 0x6d, 0x65, 0x6e, 0x74, 0x20, 0x6c, 0x6f, 0x6e, 0x67, 0x20, 0x70,
            0x6f, 0x75, 0x72, 0x20, 0x63, 0x68, 0x61, 0x6e, 0x67, 0x65, 0x72, 0x20, 0x64, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6d, 0x61, 0x74, 0x20, 0x2d, 0x20,
            0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x76, 0x40};

        auto functionCall = msg::FunctionCall<>::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
        REQUIRE(functionCall->getParametersCount() == 4);
        REQUIRE(functionCall->getPayloadSize() == 433);
        auto [name, undecodedData] = functionCall->getFunctionName();
        REQUIRE(name == "cppTest");
        std::string text;
        functionCall->decodeParameter(text, undecodedData);
        REQUIRE(text == "Texte de test suffisament long pour changer de format");

        functionCall->decodeParameter(text, undecodedData);
        REQUIRE(text.starts_with("bigText : Texte de test suffisament long pour changer de format"));
        REQUIRE(text.size() == 357);
        REQUIRE(text.ends_with("suffisament long pour changer de format - "));

        size_t value{};
        functionCall->decodeParameter(value, undecodedData);
        REQUIRE(value == text.size());
        REQUIRE(undecodedData.empty());
    }
}

SCENARIO("FunctionReturn") {
    using Net = networking::NetworkingMock;

    GIVEN("A FunctionReturn message") {
        msg::FunctionReturn<>     message;
        websocket::Frame<Net>     frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
        networking::SocketMock    socket;
        auto ws = websocket::WebSocket<Net>::create(socket);

        THEN("Header command byte is functionReturn") {
            REQUIRE(static_cast<msg::Command>(message.header()[0]) == msg::Command::functionReturn);
        }

        WHEN("An exception is encoded") {
            std::string exceptionText = "Parameter error";
            auto        exception     = std::runtime_error(exceptionText);
            message.encodeParameter(exception, frame);
            ws->write(std::move(frame));

            THEN("Encoded frame should be") {
                auto encodedFrame = span(socket.debugBuffer.data(), socket.bufferIndex);
                REQUIRE(encodedFrame.size() == 2 + message.header().size() + 3 + exceptionText.size());
            }

            THEN("A Frame decoded should retrieve the encoded parameters") {
                websocket::FrameDecoder decoder;
                REQUIRE(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));

                auto funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                REQUIRE(funcRet->getParametersCount() == 1);
                REQUIRE(funcRet->getPayloadSize() == 3 + exceptionText.size());

                std::string text;
                auto        undecodedData = funcRet->payload();
                funcRet->decodeParameter(text, undecodedData);
                REQUIRE(text == exceptionText);
                REQUIRE(undecodedData.empty());
            }
        }

        WHEN("A tuple is encoded") {
            std::tuple<int, std::string> value{42, "Hello World"};
            message.encodeParameter(value, frame);
            ws->write(std::move(frame));
            cout << "Socket wrote :\n" << utils::hexDump(span(socket.debugBuffer.data(), socket.bufferIndex)) << '\n';

            THEN("An erroneous tuple should trigger an exception") {
                websocket::FrameDecoder decoder;
                REQUIRE(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));
                auto                              funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                std::tuple<int, std::string, int> tupleValue;
                auto                              undecodedData = funcRet->payload();
                REQUIRE_THROWS_AS(funcRet->decodeParameter(tupleValue, undecodedData), std::runtime_error);
            }
            THEN("A Frame decoded should retrieve the encoded parameters") {
                websocket::FrameDecoder decoder;
                REQUIRE(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));
                auto funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                REQUIRE(funcRet->getParametersCount() == 3);
                REQUIRE(funcRet->getPayloadSize() == 24);

                std::tuple<int, std::string> tupleValue;
                auto                         undecodedData = funcRet->payload();
                funcRet->decodeParameter(tupleValue, undecodedData);
                REQUIRE(std::get<0>(tupleValue) == 42);
                REQUIRE(std::get<1>(tupleValue) == "Hello World");
                std::cout << undecodedData.size() << "\n";
                REQUIRE(undecodedData.empty());
            }
        }
    }
}

SCENARIO("Function call correlation identifiers") {
    msg::FunctionCall<> message;
    message.setCallId(42);
    REQUIRE(message.getCallId() == 42);
    message.reset();
    REQUIRE(message.getCallId() == 0);
}

SCENARIO("Numeric arrays are encoded and decoded as owning values") {
    using Net = networking::NetworkingMock;

    auto roundTrip = []<typename Element>(const vector<Element>& input) {
        msg::FunctionReturn<>     message;
        websocket::Frame<Net>     frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
        networking::SocketMock    socket;
        auto ws = websocket::WebSocket<Net>::create(socket);

        message.encodeParameter(input, frame);
        ws->write(std::move(frame));

        websocket::FrameDecoder decoder;
        REQUIRE(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));
        const auto*     decodedMessage = msg::FunctionReturn<>::castFromRawData(decoder.payload());
        auto            payload        = decodedMessage->payload();
        vector<Element> output;
        decodedMessage->decodeParameter(output, payload);
        REQUIRE(output == input);
        REQUIRE(payload.empty());
    };

    roundTrip(vector<uint8_t>{});
    roundTrip(vector<uint8_t>{0, 1, 127, 255});
    roundTrip(vector<int8_t>{-128, -1, 0, 127});
    roundTrip(vector<uint16_t>{0, 1, 65535});
    roundTrip(vector<int16_t>{-32768, -1, 32767});
    roundTrip(vector<uint32_t>{0, 1, 0xffffffffu});
    roundTrip(vector<int32_t>{numeric_limits<int32_t>::min(), -1, numeric_limits<int32_t>::max()});
    roundTrip(vector<uint64_t>{0, 1, numeric_limits<uint64_t>::max()});
    roundTrip(vector<int64_t>{numeric_limits<int64_t>::min(), -1, numeric_limits<int64_t>::max()});
    roundTrip(vector<float>{-1.5F, 0.0F, 42.25F});
    roundTrip(vector<double>{-1.5, 0.0, 42.25});

}

SCENARIO("Uint8 arrays switch encoding at the compact-length boundary") {
    using Net = networking::NetworkingMock;

    auto encodedType = []<typename Array>(const Array& input) {
        msg::FunctionReturn<> message;
        websocket::Frame<Net> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
        message.encodeParameter(input, frame);
        const auto buffers = frame.toBuffers();
        REQUIRE(buffers.size() == 4);
        const auto type = *reinterpret_cast<const byte*>(buffers[2].data());
        return static_cast<msg::CodedType>(to_integer<uint8_t>(type));
    };

    vector<uint8_t>     compact(255, 7);
    vector<uint8_t>     regular(256, 9);
    array<uint8_t, 255> fixedCompact{};
    array<uint8_t, 256> fixedRegular{};

    REQUIRE(encodedType(compact) == msg::CodedType::smallArrayU8);
    REQUIRE(encodedType(regular) == msg::CodedType::arrayU8);
    REQUIRE(encodedType(fixedCompact) == msg::CodedType::smallArrayU8);
    REQUIRE(encodedType(fixedRegular) == msg::CodedType::arrayU8);
}

namespace {
/// Three uint16 elements (1, 2, 3) in an arrayU16 payload, little-endian.
constexpr array<byte, 11> threeUint16s{byte{static_cast<uint8_t>(msg::CodedType::arrayU16)},
                                       byte{3},
                                       byte{0},
                                       byte{0},
                                       byte{0},
                                       byte{1},
                                       byte{0},
                                       byte{2},
                                       byte{0},
                                       byte{3},
                                       byte{0}};
}  // namespace

SCENARIO("Numeric arrays decode into fixed-size destinations") {
    array<uint16_t, 3> output{};
    span<const byte>   payload{threeUint16s};
    msg::FunctionCall<>::decodeParameter(output, payload);
    REQUIRE(output == array<uint16_t, 3>{1, 2, 3});
    REQUIRE(payload.empty());
}

SCENARIO("Malformed numeric array payloads are rejected") {
    GIVEN("A payload shorter than its declared element count") {
        vector<uint8_t>  output;
        array<byte, 3>   truncated{byte{static_cast<uint8_t>(msg::CodedType::smallArrayU8)}, byte{2}, byte{1}};
        span<const byte> payload{truncated};
        REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(output, payload), runtime_error);
    }

    GIVEN("A fixed-size destination whose length disagrees with the payload") {
        array<uint16_t, 2> fixed{};
        span<const byte>   payload{threeUint16s};
        REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(fixed, payload), runtime_error);
    }

    GIVEN("A wire type that does not match the destination element type") {
        vector<uint32_t> output;
        span<const byte> payload{threeUint16s};
        REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(output, payload), runtime_error);
    }

    GIVEN("An array header truncated before its length field") {
        vector<uint16_t> output;
        array<byte, 2>   shortHeader{byte{static_cast<uint8_t>(msg::CodedType::arrayU16)}, byte{1}};
        span<const byte> payload{shortHeader};
        REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(output, payload), runtime_error);
    }
}

SCENARIO("decodeParameter rejects malformed and mistyped payloads") {
    auto decodeInto = []<typename T>(T& target, span<const byte> bytes) {
        span<const byte> payload{bytes};
        msg::FunctionCall<>::decodeParameter(target, payload);
    };
    auto coded = [](msg::CodedType type) { return byte{static_cast<uint8_t>(type)}; };

    GIVEN("An empty payload") {
        string           target;
        span<const byte> payload{};
        REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(target, payload), runtime_error);
    }

    GIVEN("An unknown coded type") {
        string        target;
        array<byte, 1> unknown{byte{0xfe}};
        REQUIRE_THROWS_AS(decodeInto(target, unknown), runtime_error);
    }

    GIVEN("A value decoded into the wrong destination type") {
        // Every mismatch must throw: silently leaving the bytes unconsumed would desynchronise
        // the parameters that follow it in the same message.
        array<byte, 1> boolean{coded(msg::CodedType::booleanTrue)};
        array<byte, 2> smallString{coded(msg::CodedType::smallString), byte{0}};
        array<byte, 9> number{coded(msg::CodedType::number)};
        array<byte, 2> tuple{coded(msg::CodedType::tuple), byte{1}};
        array<byte, 2> array8{coded(msg::CodedType::smallArrayU8), byte{0}};

        string          asString;
        bool            asBool{};
        int             asInt{};
        vector<uint8_t> asVector;

        REQUIRE_THROWS_AS(decodeInto(asString, boolean), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(asBool, smallString), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(asString, number), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(asInt, tuple), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(asInt, array8), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(asVector, boolean), runtime_error);
    }

    GIVEN("String payloads truncated after their type byte") {
        string         target;
        array<byte, 1> smallHeader{coded(msg::CodedType::smallString)};
        array<byte, 2> longHeader{coded(msg::CodedType::string), byte{4}};
        array<byte, 3> smallBody{coded(msg::CodedType::smallString), byte{8}, byte{'a'}};
        array<byte, 4> longBody{coded(msg::CodedType::string), byte{8}, byte{0}, byte{'a'}};

        REQUIRE_THROWS_AS(decodeInto(target, smallHeader), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(target, longHeader), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(target, smallBody), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(target, longBody), runtime_error);
    }

    GIVEN("A number payload shorter than a double") {
        double         target{};
        array<byte, 4> shortNumber{coded(msg::CodedType::number), byte{0}, byte{0}, byte{0}};
        REQUIRE_THROWS_AS(decodeInto(target, shortNumber), runtime_error);
    }

    GIVEN("A tuple whose declared arity differs from the destination") {
        tuple<int, string> target;
        array<byte, 2>     wrongArity{coded(msg::CodedType::tuple), byte{3}};
        array<byte, 1>     truncatedHeader{coded(msg::CodedType::tuple)};
        REQUIRE_THROWS_AS(decodeInto(target, wrongArity), runtime_error);
        REQUIRE_THROWS_AS(decodeInto(target, truncatedHeader), runtime_error);
    }
}

SCENARIO("BufferOverflow") {
    using Net        = networking::NetworkingMock;
    using TinyPolicy = http::CustomBuffersPolicy<8192, 8192, 12>;

    GIVEN("A FunctionCall with a tiny buffer") {
        msg::FunctionCall<TinyPolicy> message;
        websocket::Frame<Net>         frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};

        WHEN("Number overflows the buffer") {
            message.encodeParameter("fn", frame);  // 2 bytes in buffer (type + uint8 size)
            message.encodeParameter(1, frame);     // 9 bytes in buffer (type + double) → total 11

            THEN("Next number overflows") {
                REQUIRE_THROWS_AS(message.encodeParameter(2, frame), http::BufferOverflowException);
            }
        }

        WHEN("Boolean overflows the buffer") {
            message.encodeParameter("fn", frame);  // 2 bytes → total 2
            message.encodeParameter(1, frame);     // 9 bytes → total 11
            message.encodeParameter(true, frame);  // 1 byte  → total 12 (exactly full)

            THEN("Next boolean overflows") {
                REQUIRE_THROWS_AS(message.encodeParameter(false, frame), http::BufferOverflowException);
            }
        }

        WHEN("String header overflows the buffer") {
            message.encodeParameter("fn", frame);  // 2 bytes → total 2
            message.encodeParameter(1, frame);     // 9 bytes → total 11

            THEN("Next string header overflows") {
                REQUIRE_THROWS_AS(message.encodeParameter("x", frame), http::BufferOverflowException);
            }
        }
    }
}
