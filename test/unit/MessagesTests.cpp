#include "InlineTaskScope.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/tooling/HexDump.hpp>
#include <webfront/weblink/Messages.hpp>

#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace std;
using namespace webfront;

SCENARIO("Handshake message") {
    std::array<uint8_t, 2> raw{0x00, 0x00};
    auto                   handshake = msg::Handshake::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));

    REQUIRE(sizeof(*handshake) == 2);
    REQUIRE(handshake->getEndian() == msg::JSEndian::little);
}

SCENARIO("Handshake message with the proof of an embedded view") {
    // ADR-003: bytes after the two header bytes are the launch secret of ADR-020; the header itself is unchanged.
    const std::string proof(64, 'a');
    std::vector<std::byte> raw{std::byte{0x00}, std::byte{0x00}};
    for (const char c : proof)
        raw.push_back(static_cast<std::byte>(c));
    auto handshake = msg::Handshake::castFromRawData(std::span(raw));
    REQUIRE(handshake->getEndian() == msg::JSEndian::little);
    REQUIRE(handshake->getPayloadSize() == 0);
    REQUIRE(raw.size() - sizeof(*handshake) == proof.size());
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
        webfront::testing::InlineTaskScope scope;
        auto                               ws = websocket::WebSocket<Net>::create(socket, scope);

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
                REQUIRE(encodedFrame.size() == 2 + message.header().size() + 4 + bridge_code::generic.size() + exceptionText.size());
            }

            THEN("A Frame decoded should retrieve the encoded parameters") {
                websocket::FrameDecoder decoder;
                REQUIRE(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));

                auto funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                REQUIRE(funcRet->getParametersCount() == 1);
                REQUIRE(funcRet->getPayloadSize() == 4 + bridge_code::generic.size() + exceptionText.size());

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
                REQUIRE(funcRet->getParametersCount() == 1);
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

namespace {

// The message bytes a frame carries, without the WebSocket header.
vector<byte> messageBytes(const websocket::Frame<networking::NetworkingMock>& frame) {
    vector<byte> bytes;
    const auto   buffers = frame.toBuffers();
    for (auto buffer = next(buffers.begin()); buffer != buffers.end(); ++buffer) {
        const auto* first = static_cast<const byte*>(buffer->data());
        bytes.insert(bytes.end(), first, first + buffer->size());
    }
    return bytes;
}

}  // namespace

// Encodes 'value' alone in a function return and decodes it back as a string.
template <typename T>
string roundTrip(const T& value) {
    msg::FunctionReturn<>                        message;
    websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(value, frame);
    const auto bytes   = messageBytes(frame);
    auto       decoded = msg::FunctionReturn<>::castFromRawData(bytes);
    auto       payload = decoded->payload();
    string     text;
    decoded->decodeParameter(text, payload);
    if (!payload.empty())
        throw runtime_error("Trailing data after the decoded string");
    return text;
}

namespace {
/// The coded type byte that opens the first parameter of a return message holding 'value'.
template <typename T>
msg::CodedType firstCodeOf(const T& value) {
    msg::FunctionReturn<>                        message;
    websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(value, frame);
    return static_cast<msg::CodedType>(messageBytes(frame).at(8));
}
}  // namespace

SCENARIO("Strings of 65536 bytes or more use a 32-bit length") {
    GIVEN("strings on both sides of the 16-bit boundary") {
        THEN("255 bytes is a smallString, 256 to 65535 a string and 65536 a longString") {
            REQUIRE(firstCodeOf(string(255, 'x')) == msg::CodedType::smallString);
            REQUIRE(firstCodeOf(string(256, 'x')) == msg::CodedType::string);
            REQUIRE(firstCodeOf(string(65535, 'x')) == msg::CodedType::string);
            REQUIRE(firstCodeOf(string(65536, 'x')) == msg::CodedType::longString);
        }
        THEN("each crosses the wire whole") {
            for (const auto size : {255uz, 256uz, 65535uz, 65536uz, 65537uz, 1000000uz}) {
                const string text(size, 'x');
                REQUIRE(roundTrip(text) == text);
            }
        }
    }
    GIVEN("a UTF-8 character that straddles the 65535-byte boundary") {
        const string text = string(65534, 'x') + "\xc3\xa9" + string(10, 'y');  // 'é' occupies bytes 65534 and 65535
        THEN("it is not cut: the whole string is a longString") {
            REQUIRE(text.size() == 65546);
            REQUIRE(firstCodeOf(text) == msg::CodedType::longString);
            REQUIRE(roundTrip(text) == text);
        }
    }
    GIVEN("a string whose multi-byte characters take it past 65535 bytes") {
        string text;
        for (int i = 0; i < 40000; ++i)
            text += "\xe2\x82\xac";  // the euro sign, 3 bytes
        THEN("the size counts bytes, not characters") {
            REQUIRE(roundTrip(text) == text);
        }
    }
    GIVEN("a longString header whose payload is truncated or inconsistent") {
        auto decode = [](span<const byte> bytes) {
            string           target;
            span<const byte> payload{bytes};
            msg::FunctionCall<>::decodeParameter(target, payload);
        };
        const auto long5 = [](uint32_t size, size_t present) {
            vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::longString)}};
            for (int i = 0; i < 4; ++i)
                bytes.push_back(byte{static_cast<uint8_t>(size >> (8 * i))});
            bytes.insert(bytes.end(), present, byte{'a'});
            return bytes;
        };
        THEN("a header shorter than five bytes is refused") {
            const vector<byte> header{byte{static_cast<uint8_t>(msg::CodedType::longString)}, byte{1}, byte{0}};
            REQUIRE_THROWS_AS(decode(header), runtime_error);
        }
        THEN("a length that exceeds the bytes present is refused") {
            REQUIRE_THROWS_AS(decode(long5(70000, 69999)), runtime_error);
            REQUIRE_THROWS_AS(decode(long5(0xffffffffu, 10)), runtime_error);
        }
        THEN("the exact length decodes and leaves the cursor after it") {
            auto             bytes = long5(70000, 70000);
            string           target;
            span<const byte> payload{bytes};
            msg::FunctionCall<>::decodeParameter(target, payload);
            REQUIRE(target.size() == 70000);
            REQUIRE(payload.empty());
        }
        THEN("a destination that is not a string is refused") {
            auto bytes = long5(5, 5);
            int  number{};
            span<const byte> payload{bytes};
            REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(number, payload), runtime_error);
        }
    }
    GIVEN("a policy whose messages cannot exceed 1 MiB") {
        using SmallPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 1024 * 1024>;
        msg::FunctionReturn<SmallPolicy>             message;
        websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
        THEN("a string that fits is encoded") {
            message.encodeParameter(string(1024 * 1024 - 8 - 5, 'x'), frame);
            REQUIRE(message.getPayloadSize() == 1024 * 1024 - 8);
        }
        THEN("a string one byte too large throws length_error and leaves the message untouched") {
            REQUIRE_THROWS_AS(message.encodeParameter(string(1024 * 1024 - 8 - 4, 'x'), frame), length_error);
            REQUIRE(message.getPayloadSize() == 0);
            REQUIRE(message.getParametersCount() == 0);
            REQUIRE(frame.size() == message.header().size());
        }
        THEN("the bound counts what the message already carries") {
            message.encodeParameter(string(600 * 1024, 'x'), frame);
            REQUIRE_THROWS_AS(message.encodeParameter(string(600 * 1024, 'y'), frame), length_error);
        }
    }
    GIVEN("an exception message longer than 65535 bytes, with a two-byte character across the limit") {
        const runtime_error error{string(65534, 'x') + "\xc3\xa9" + string(10, 'y')};
        THEN("the message is cut before the first character that does not fit whole") {
            REQUIRE(roundTrip(error) == string(65534, 'x'));
        }
    }
}

namespace {

/// The bytes of 'value' alone as the parameter of a function return, without the message header.
template <typename T>
vector<byte> encodedParameter(const T& value) {
    msg::FunctionReturn<>                        message;
    websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(value, frame);
    auto bytes = messageBytes(frame);
    return vector<byte>(bytes.begin() + 8, bytes.end());
}

template <typename T>
T decodedParameter(const vector<byte>& bytes) {
    T                value{};
    span<const byte> payload{bytes};
    msg::FunctionCall<>::decodeParameter(value, payload);
    if (!payload.empty())
        throw runtime_error("Trailing data after the decoded list");
    return value;
}

template <typename T>
T listRoundTrip(const T& value) {
    return decodedParameter<T>(encodedParameter(value));
}

vector<byte> listBytes(uint32_t count, initializer_list<vector<byte>> elements) {
    vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::list)}};
    for (int i = 0; i < 4; ++i)
        bytes.push_back(byte{static_cast<uint8_t>(count >> (8 * i))});
    for (const auto& element : elements)
        bytes.insert(bytes.end(), element.begin(), element.end());
    return bytes;
}

vector<byte> wireString(string_view text) {
    vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::smallString)}, byte{static_cast<uint8_t>(text.size())}};
    for (const char c : text)
        bytes.push_back(byte{static_cast<uint8_t>(c)});
    return bytes;
}

vector<byte> wireNumber(double value) {
    vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::number)}};
    const auto*  raw = reinterpret_cast<const byte*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
    return bytes;
}

static_assert(msg::is_list_v<vector<string>>);
static_assert(msg::is_list_v<vector<tuple<string, int, bool>>>);
static_assert(msg::is_list_v<vector<vector<uint8_t>>>);
static_assert(!msg::is_list_v<vector<int>>, "a vector of numbers is a typed array");
static_assert(!msg::is_list_v<vector<vector<string>>>, "lists do not nest");
static_assert(!msg::is_list_v<vector<bool>>);
static_assert(msg::contains_list_v<tuple<int, vector<string>>>);

}  // namespace

SCENARIO("Lists of strings, tuples and arrays are encoded as one list value") {
    GIVEN("an empty list of strings") {
        const vector<string> empty;
        THEN("it is the list code and a zero count") {
            REQUIRE(encodedParameter(empty) == listBytes(0, {}));
            REQUIRE(listRoundTrip(empty).empty());
        }
    }
    GIVEN("a list with one element, then several of different lengths") {
        const vector<string> one{"alpha"};
        const vector<string> several{"", "beta", string(300, 'g'), string(70000, 'd')};
        THEN("elements keep their order and their own string header") {
            REQUIRE(encodedParameter(one) == listBytes(1, {wireString("alpha")}));
            REQUIRE(listRoundTrip(one) == one);
            REQUIRE(listRoundTrip(several) == several);
        }
        THEN("the list is one parameter, not one per element") {
            msg::FunctionReturn<>                        message;
            websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
            message.encodeParameter(several, frame);
            REQUIRE(message.getParametersCount() == 1);
        }
    }
    GIVEN("a list of records mixing strings, numbers, booleans and a Uint8Array") {
        using Record = tuple<string, double, bool, vector<uint8_t>>;
        const vector<Record> entries{{"a.txt", 12.5, true, {1, 2, 3}}, {"b", 0, false, {}}, {"c", -1, true, vector<uint8_t>(300, 7)}};
        THEN("every field of every record crosses the wire") {
            REQUIRE(listRoundTrip(entries) == entries);
        }
    }
    GIVEN("a list of typed arrays and a list of bitsets") {
        const vector<vector<uint8_t>> chunks{{1}, vector<uint8_t>(500, 9), {}};
        const vector<bitset<9>>       bits{bitset<9>{0x181}, bitset<9>{}};
        THEN("each is an element of its list") {
            REQUIRE(listRoundTrip(chunks) == chunks);
            REQUIRE(listRoundTrip(bits) == bits);
        }
    }
    GIVEN("a tuple that holds a list, as a page of results with its cursor does") {
        const tuple<vector<string>, string> page{{"x", "y"}, "next"};
        THEN("the list is nested in the tuple, not in a list") {
            REQUIRE(listRoundTrip(page) == page);
        }
    }
    GIVEN("a list sent by JavaScript as a plain array, hence a tuple") {
        vector<byte> tupleBytes{byte{static_cast<uint8_t>(msg::CodedType::tuple)}, byte{2}};
        for (const auto& element : {wireString("one"), wireString("two")})
            tupleBytes.insert(tupleBytes.end(), element.begin(), element.end());
        THEN("it decodes into a vector of the same elements") {
            REQUIRE(decodedParameter<vector<string>>(tupleBytes) == vector<string>{"one", "two"});
        }
        THEN("a tuple of other elements is still refused") {
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(vector<byte>{byte{static_cast<uint8_t>(msg::CodedType::tuple)}, byte{1}, wireNumber(1)[0], wireNumber(1)[1]}),
                              runtime_error);
        }
    }
    GIVEN("lists that are heterogeneous, truncated or inconsistent") {
        THEN("an element of another type than the first is refused") {
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(listBytes(2, {wireString("a"), wireNumber(1)})), runtime_error);
            REQUIRE_THROWS_AS((decodedParameter<vector<tuple<string, int>>>(listBytes(2, {encodedParameter(tuple<string, int>{"a", 1}), wireString("b")}))),
                              runtime_error);
        }
        THEN("a tuple of another arity than the first is refused") {
            REQUIRE_THROWS_AS((decodedParameter<vector<tuple<string, int>>>(
                                   listBytes(2, {encodedParameter(tuple<string, int>{"a", 1}), encodedParameter(tuple<string>{"b"})}))),
                              runtime_error);
        }
        THEN("a list or an exception inside a list is refused") {
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(listBytes(1, {listBytes(0, {})})), runtime_error);
            const vector<byte> exception{byte{static_cast<uint8_t>(msg::CodedType::exception)}, byte{1}, byte{0}, byte{'x'}};
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(listBytes(1, {exception})), runtime_error);
        }
        THEN("a count larger than the bytes that follow is refused without allocating for it") {
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(listBytes(0xffffffffu, {wireString("a")})), runtime_error);
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(listBytes(3, {wireString("a"), wireString("b")})), runtime_error);
        }
        THEN("a header shorter than five bytes, or an element cut short, is refused") {
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(vector<byte>{byte{static_cast<uint8_t>(msg::CodedType::list)}, byte{1}}), runtime_error);
            auto cut = listBytes(1, {wireString("abcdef")});
            cut.pop_back();
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(cut), runtime_error);
        }
        THEN("a list decoded into another kind of destination is refused") {
            REQUIRE_THROWS_AS(decodedParameter<string>(listBytes(0, {})), runtime_error);
            REQUIRE_THROWS_AS(decodedParameter<vector<int>>(listBytes(0, {})), runtime_error);
            REQUIRE_THROWS_AS(decodedParameter<vector<string>>(encodedParameter(vector<uint8_t>{1})), runtime_error);
        }
    }
    GIVEN("elements whose values decode into the destination but whose wire shapes differ") {
        const auto boolTuple   = encodedParameter(tuple<bool>{true});
        const auto numberTuple = encodedParameter(tuple<double>{1});
        THEN("a list of [true] and [1] is refused although a number decodes as a bool") {
            REQUIRE_THROWS_AS(decodedParameter<vector<tuple<bool>>>(listBytes(2, {boolTuple, numberTuple})), runtime_error);
        }
        THEN("the same holds for a plain JavaScript array, which arrives as a tuple") {
            vector<byte> asTuple{byte{static_cast<uint8_t>(msg::CodedType::tuple)}, byte{2}};
            for (const auto* element : {&boolTuple, &numberTuple})
                asTuple.insert(asTuple.end(), element->begin(), element->end());
            REQUIRE_THROWS_AS(decodedParameter<vector<tuple<bool>>>(asTuple), runtime_error);
        }
        THEN("true and false are one shape") {
            REQUIRE(decodedParameter<vector<tuple<bool>>>(listBytes(2, {boolTuple, encodedParameter(tuple<bool>{false})})).size() == 2);
        }
        THEN("different fields at the same position are refused, equal shapes with different string headers are accepted") {
            REQUIRE_THROWS_AS((decodedParameter<vector<tuple<string, double>>>(
                                   listBytes(2, {encodedParameter(tuple<string, double>{"a", 1}), encodedParameter(tuple<double, string>{2, "b"})}))),
                              runtime_error);
            const vector<tuple<string, double>> mixed{{"a", 1}, {string(300, 'b'), 2}, {string(70000, 'c'), 3}};
            REQUIRE(listRoundTrip(mixed) == mixed);
        }
        THEN("the short and the regular Uint8Array are one shape, other element types are not") {
            const vector<vector<uint8_t>> bytes{vector<uint8_t>(3, 1), vector<uint8_t>(300, 2)};
            REQUIRE(listRoundTrip(bytes) == bytes);
            REQUIRE_THROWS_AS(decodedParameter<vector<vector<uint8_t>>>(listBytes(2, {encodedParameter(vector<uint8_t>{1}), encodedParameter(vector<int8_t>{1})})),
                              runtime_error);
        }
    }
    GIVEN("tuples nested far deeper than any application type") {
        // A list whose first element is 100000 tuples of one field each: 2 bytes of header per level.
        vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::list)}, byte{1}, byte{0}, byte{0}, byte{0}};
        for (int level = 0; level < 100000; ++level) {
            bytes.push_back(byte{static_cast<uint8_t>(msg::CodedType::tuple)});
            bytes.push_back(byte{1});
        }
        bytes.push_back(byte{static_cast<uint8_t>(msg::CodedType::booleanTrue)});
        THEN("the shape check refuses them before the stack is at risk") {
            REQUIRE_THROWS_AS(decodedParameter<vector<tuple<bool>>>(bytes), runtime_error);
        }
    }
    GIVEN("a policy whose messages cannot exceed 1 KiB") {
        using SmallPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 1024>;
        msg::FunctionReturn<SmallPolicy>             message;
        websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
        THEN("a list that does not fit throws length_error") {
            REQUIRE_THROWS_AS(message.encodeParameter(vector<string>(200, string(10, 'x')), frame), length_error);
        }
    }
    GIVEN("a policy whose header buffer is small") {
        using TinyPolicy = http::CustomBuffersPolicy<8192, 8192, 64>;
        msg::FunctionReturn<TinyPolicy>              message;
        websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
        THEN("a list with more element headers than the buffer holds overflows explicitly") {
            REQUIRE_THROWS_AS(message.encodeParameter(vector<string>(100, "x"), frame), http::BufferOverflowException);
        }
    }
}

SCENARIO("A typed array that exactly fills the message size is encoded") {
    using ExactPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 32>;
    msg::FunctionReturn<ExactPolicy>             message;
    websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
    THEN("its header is counted once") {
        const vector<uint8_t> bytes(22, 1);  // 8 header bytes + 2 + 22 = 32
        REQUIRE_NOTHROW(message.encodeParameter(bytes, frame));
        REQUIRE(message.getPayloadSize() == 24);
    }
    THEN("one more byte is refused") {
        REQUIRE_THROWS_AS(message.encodeParameter(vector<uint8_t>(23, 1), frame), length_error);
    }
}

SCENARIO("Exceptions carry a code and a message") {
    auto decodeBoth = [](const vector<byte>& bytes) {
        string           code, message;
        span<const byte> payload{bytes};
        msg::FunctionReturn<>::decodeException(code, message, payload);
        REQUIRE(payload.empty());
        return pair<string, string>{code, message};
    };
    GIVEN("an application error") {
        const BridgeError error{"quota:exceeded", "no space left"};
        THEN("the wire carries the code, then the message") {
            const auto bytes = encodedParameter(error);
            REQUIRE(bytes.size() == 2 + 14 + 2 + 13);
            REQUIRE(bytes[0] == byte{static_cast<uint8_t>(msg::CodedType::exception)});
            REQUIRE(bytes[1] == byte{14});
            REQUIRE(decodeBoth(bytes) == pair<string, string>{"quota:exceeded", "no space left"});
        }
        THEN("decoded as a string, the exception is its message") {
            REQUIRE(roundTrip(error) == "no space left");
        }
    }
    GIVEN("an ordinary exception") {
        THEN("it is sent with the generic code") {
            REQUIRE(decodeBoth(encodedParameter(runtime_error{"boom"})) == pair<string, string>{string(bridge_code::generic), "boom"});
            REQUIRE(decodeBoth(encodedParameter(out_of_range{"gone"})).first == bridge_code::generic);
        }
    }
    GIVEN("an error of the bridge") {
        THEN("its reserved code crosses the wire") {
            REQUIRE(decodeBoth(encodedParameter(detail::makeBridgeError(bridge_code::cancelled, "stopped"))).first == bridge_code::cancelled);
        }
    }
    GIVEN("an exception message longer than 65535 bytes") {
        THEN("the code is kept and the message is cut before a character that does not fit") {
            const BridgeError error{"big", string(65534, 'x') + "\xc3\xa9" + string(10, 'y')};
            REQUIRE(decodeBoth(encodedParameter(error)) == pair<string, string>{"big", string(65534, 'x')});
        }
    }
    GIVEN("a truncated exception") {
        auto bytes = encodedParameter(BridgeError{"code", "message"});
        THEN("every cut is refused") {
            for (size_t keep = 0; keep < bytes.size(); ++keep) {
                const vector<byte> cut(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(keep));
                string             code, message;
                span<const byte>   payload{cut};
                REQUIRE_THROWS_AS(msg::FunctionReturn<>::decodeException(code, message, payload), runtime_error);
            }
        }
    }
    GIVEN("a code that is ill-formed or reserved") {
        THEN("an application cannot build the error") {
            REQUIRE_THROWS_AS(BridgeError("", "m"), invalid_argument);
            REQUIRE_THROWS_AS(BridgeError(string(65, 'a'), "m"), invalid_argument);
            REQUIRE_THROWS_AS(BridgeError("with space", "m"), invalid_argument);
            REQUIRE_THROWS_AS(BridgeError("webfront.cancelled", "m"), invalid_argument);
            REQUIRE_NOTHROW(BridgeError(string(64, 'a'), "m"));
        }
        THEN("a peer's ill-formed code becomes the generic one") {
            REQUIRE(detail::makeBridgeError("bad code", "m").code() == bridge_code::generic);
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
        webfront::testing::InlineTaskScope scope;
        auto                               ws = websocket::WebSocket<Net>::create(socket, scope);

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

SCENARIO("Empty wire strings are decoded without indexing past the input") {
    for (auto bytes : {
             std::vector<std::byte>{std::byte{4}, std::byte{0}},
             std::vector<std::byte>{std::byte{5}, std::byte{0}, std::byte{0}}
    }) {
        std::span<const std::byte> input(bytes);
        std::string                value = "previous";
        msg::FunctionCall<>::decodeParameter(value, input);
        REQUIRE(value.empty());
        REQUIRE(input.empty());
    }
}

SCENARIO("Wire numbers outside an integer result range are rejected") {
    for (double number : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 2147483648.0, -2147483649.0}) {
        std::array<std::byte, 9> bytes{std::byte{3}};
        std::memcpy(bytes.data() + 1, &number, sizeof(number));
        std::span<const std::byte> input(bytes);
        int                        value{};
        REQUIRE_THROWS_AS(msg::FunctionCall<>::decodeParameter(value, input), std::runtime_error);
    }
}

SCENARIO("Wire integer conversion keeps truncation and handles 64-bit boundaries") {
    auto decode = []<typename T>(double number, T& value) {
        std::array<std::byte, 9> bytes{std::byte{3}};
        std::memcpy(bytes.data() + 1, &number, sizeof(number));
        std::span<const std::byte> input(bytes);
        msg::FunctionCall<>::decodeParameter(value, input);
        REQUIRE(input.empty());
    };
    int value{};
    decode(42.75, value);
    REQUIRE(value == 42);
    decode(-42.75, value);
    REQUIRE(value == -42);
    decode(2147483647.75, value);
    REQUIRE(value == std::numeric_limits<int>::max());

    std::uint64_t unsignedValue{};
    REQUIRE_THROWS_AS(decode(18446744073709551616.0, unsignedValue), std::runtime_error);
    REQUIRE_THROWS_AS(decode(-1.0, unsignedValue), std::runtime_error);
    decode(9223372036854775808.0, unsignedValue);
    REQUIRE(unsignedValue == (std::uint64_t{1} << 63));
    std::int64_t signedValue{};
    REQUIRE_THROWS_AS(decode(9223372036854775808.0, signedValue), std::runtime_error);
    decode(-9223372036854775808.0, signedValue);
    REQUIRE(signedValue == std::numeric_limits<std::int64_t>::min());
}

namespace {
template <typename... Values>
vector<byte> bitsetPayload(const Values&... values) {
    msg::FunctionReturn<>                        message;
    websocket::Frame<networking::NetworkingMock> frame{message.header()};
    (message.encodeParameter(values, frame), ...);
    REQUIRE(message.getParametersCount() == sizeof...(Values));
    vector<byte> payload;
    const auto   buffers = frame.toBuffers();
    for (size_t i = 2; i < buffers.size(); ++i) {
        const auto* start = static_cast<const byte*>(buffers[i].data());
        if (buffers[i].size())
            payload.insert(payload.end(), start, start + buffers[i].size());
    }
    REQUIRE(payload.size() == message.getPayloadSize());
    return payload;
}

template <size_t N>
void expectPackedBitsetBytes(span<const byte> bytes, const bitset<N>& input) {
    constexpr size_t count = N / 8 + (N % 8 != 0);
    constexpr size_t header = count < 256 ? 2 : 5;
    if constexpr (N != 0)
        for (size_t b = 0; b < count; ++b) {
            unsigned expected = 0;
            for (size_t bit = 0; bit < 8 && b * 8 + bit < N; ++bit)
                if (input[b * 8 + bit])
                    expected |= 1u << bit;
            REQUIRE(to_integer<unsigned>(bytes[header + b]) == expected);
        }
}
}  // namespace

SCENARIO("Bitsets use canonical packed Uint8Array bytes at every size boundary") {
    auto check = []<size_t N>() {
        bitset<N> input;
        if constexpr (N != 0)
            for (size_t i = 0; i < N; ++i)
                input[i] = i % 3 == 0 || i + 1 == N;
        const auto       bytes  = bitsetPayload(input);
        constexpr size_t count  = N / 8 + (N % 8 != 0);
        constexpr size_t header = count < 256 ? 2 : 5;
        REQUIRE(bytes.size() == header + count);
        REQUIRE(bytes[0] == byte(count < 256 ? msg::CodedType::smallArrayU8 : msg::CodedType::arrayU8));
        uint32_t length{};
        if constexpr (count < 256)
            length = to_integer<uint8_t>(bytes[1]);
        else
            memcpy(&length, bytes.data() + 1, sizeof(length));
        REQUIRE(length == count);
        expectPackedBitsetBytes(span<const byte>{bytes}, input);
        span<const byte> data{bytes};
        bitset<N>        output;
        output.set();
        msg::FunctionReturn<>::decodeParameter(output, data);
        // Boolean assertions avoid MSVC STL stream diagnostics for bitset<0>.
        REQUIRE((output == input));
        REQUIRE(data.empty());
        // All-one and all-zero patterns also exercise the partial last byte.
        input.set();
        const auto ones = bitsetPayload(input);
        data            = ones;
        msg::FunctionReturn<>::decodeParameter(output, data);
        REQUIRE((output == input));
        input.reset();
        const auto zeros = bitsetPayload(input);
        data             = zeros;
        msg::FunctionReturn<>::decodeParameter(output, data);
        REQUIRE(output.none());
    };
    check.template operator()<0>();
    check.template operator()<1>();
    check.template operator()<7>();
    check.template operator()<8>();
    check.template operator()<9>();
    check.template operator()<63>();
    check.template operator()<64>();
    check.template operator()<65>();
    check.template operator()<1024>();
    check.template operator()<2040>();
    check.template operator()<2048>();
    REQUIRE(bitsetPayload(bitset<9>{0x181}) == vector<byte>{byte(msg::CodedType::smallArrayU8), byte{2}, byte{0x81}, byte{1}});
}

SCENARIO("Packed bitsets retain owned bytes, tuple counts and the decoder cursor") {
    const auto expected = tuple{
        bitset<9>{0x181},
        tuple{bitset<0>{}, bitset<65>{9}},
        string("tail")
    };
    const auto       bytes = bitsetPayload(expected, bitset<8>{0xa5}, 42);
    span<const byte> data{bytes};
    remove_cvref_t<decltype(expected)> output{};
    msg::FunctionReturn<>::decodeParameter(output, data);
    REQUIRE((output == expected));
    bitset<8> bits;
    msg::FunctionReturn<>::decodeParameter(bits, data);
    REQUIRE(bits.to_ulong() == 0xa5);
    int tail{};
    msg::FunctionReturn<>::decodeParameter(tail, data);
    REQUIRE(tail == 42);
    REQUIRE(data.empty());
}

SCENARIO("Bitset decoding rejects wrong types, lengths, padding and truncation") {
    const auto valid  = bitsetPayload(bitset<9>{0x181});
    auto       reject = [](const vector<byte>& bytes) {
        span<const byte> data{bytes};
        bitset<9>        output{7};
        REQUIRE_THROWS_AS(msg::FunctionReturn<>::decodeParameter(output, data), runtime_error);
        REQUIRE(output.to_ulong() == 7);
        REQUIRE(data.size() == bytes.size());
    };
    for (size_t size = 0; size < valid.size(); ++size)
        reject(vector<byte>(valid.begin(), valid.begin() + static_cast<ptrdiff_t>(size)));
    for (auto count : {0, 1, 3}) {
        auto wrong = valid;
        wrong[1]   = byte(count);
        reject(wrong);
    }
    auto padding   = valid;
    padding.back() = byte{3};
    reject(padding);
    reject(bitsetPayload(vector<int8_t>{1, 0}));
    reject(bitsetPayload(vector<uint16_t>{1, 0}));
    reject(bitsetPayload(string("01")));
    reject(bitsetPayload(true));
    reject(bitsetPayload(3));
    reject(bitsetPayload(tuple{1, 2}));
    // A regular U8 header is valid even for a small destination.
    vector<byte> regular{byte(msg::CodedType::arrayU8), byte{2}, byte{}, byte{}, byte{}, byte{0x81}, byte{1}};
    uint32_t     count = 2;
    memcpy(regular.data() + 1, &count, sizeof(count));
    for (size_t size = 1; size < regular.size(); ++size)
        reject(vector<byte>(regular.begin(), regular.begin() + static_cast<ptrdiff_t>(size)));
    bitset<9>        output;
    span<const byte> data{regular};
    msg::FunctionReturn<>::decodeParameter(output, data);
    REQUIRE(output.to_ulong() == 0x181);
    REQUIRE(data.empty());
}

SCENARIO("Encoding bridge messages writes nothing to the standard output") {
    GIVEN("the standard output captured while a call and a return are encoded with every scalar kind") {
        ostringstream captured;
        auto*         previous = cout.rdbuf(captured.rdbuf());
        try {
            // The frame refers to the encoded strings, which therefore outlive it. Messages and frames own their buffers
            // inline: on the heap, since MSVC rejects a test function of 66 KB of stack (C6262).
            using Frame = websocket::Frame<networking::NetworkingMock>;
            const string             name("echo"), small(16, 'x'), large(1024, 'x');
            const runtime_error      failure("failure");
            const tuple<int, string> pair{1, "tuple"};
            const auto               call      = make_unique<msg::FunctionCall<>>();
            const auto               callFrame = make_unique<Frame>(span(reinterpret_cast<const byte*>(call->header().data()), call->header().size()));
            call->encodeParameter(name, *callFrame);
            call->encodeParameter(small, *callFrame);
            call->encodeParameter(large, *callFrame);
            call->encodeParameter(true, *callFrame);
            call->encodeParameter(42.5, *callFrame);
            const auto result      = make_unique<msg::FunctionReturn<>>();
            const auto resultFrame = make_unique<Frame>(span(reinterpret_cast<const byte*>(result->header().data()), result->header().size()));
            result->encodeParameter(failure, *resultFrame);
            result->encodeParameter(pair, *resultFrame);
        } catch (...) {
            cout.rdbuf(previous);
            throw;
        }
        cout.rdbuf(previous);
        THEN("the encoder printed nothing (the HTML bridge paid for console output on every message)") {
            REQUIRE(captured.str().empty());
        }
    }
}
