// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-02-13
//
// WebFront — tests/unit
// Role: Encoding and decoding of every bridge message and value type.
// Thread: bridge-io | Alloc: unrestricted
// LLM: wire format tests (ADR-003); any protocol change needs cases here

#include "InlineTaskScope.hpp"
#include <webfront/http/WebSocket.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/weblink/Messages.hpp>

#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

const Scenario handshakeAck{"Handshake and Ack messages have a two-byte header", [] {
                                require(sizeof(msg::Ack) == 2);
                                std::vector<std::byte> raw{std::byte{0x00}, std::byte{0x00}};
                                then("a bare handshake is little-endian", [&] {
                                    auto handshake = msg::Handshake::castFromRawData(std::span(raw));
                                    require(sizeof(*handshake) == 2);
                                    require(handshake->getEndian() == msg::JSEndian::little);
                                });
                                then("the proof of an embedded view follows the unchanged header", [&] {
                                    // ADR-003: bytes after the two header bytes are the launch secret of ADR-020.
                                    raw.resize(2 + 64, static_cast<std::byte>('a'));
                                    auto handshake = msg::Handshake::castFromRawData(std::span(raw));
                                    require(handshake->getEndian() == msg::JSEndian::little);
                                    require(handshake->getPayloadSize() == 0);
                                    require(raw.size() - sizeof(*handshake) == 64);
                                });
                            }};

const Scenario functionCallScenario{
    "FunctionCall",
    [] {
        given("Raw data of a call to 'print' function with a small string as parameter", [&] {
            std::array<uint8_t, 36> raw{0x03, 0x02, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x04, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x04, 0x13, 0x48,
                                        0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x20, 0x32, 0x30, 0x32, 0x32};

            auto functionCall = msg::FunctionCall<>::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
            require(functionCall->getParametersCount() == 2);
            require(functionCall->getPayloadSize() == 28);
            auto [name, undecodedData] = functionCall->getFunctionName();
            require(name == "print");
            std::string text;
            functionCall->decodeParameter(text, undecodedData);
            require(text == "Hello World of 2022");
            require(undecodedData.size() == 0);
        });
        given("Raw data of a call to 'cppTest' function with a small string, a string and a number as parameters", [&] {
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
            require(functionCall->getParametersCount() == 4);
            require(functionCall->getPayloadSize() == 433);
            auto [name, undecodedData] = functionCall->getFunctionName();
            require(name == "cppTest");
            std::string text;
            functionCall->decodeParameter(text, undecodedData);
            require(text == "Texte de test suffisament long pour changer de format");

            functionCall->decodeParameter(text, undecodedData);
            require(text.starts_with("bigText : Texte de test suffisament long pour changer de format"));
            require(text.size() == 357);
            require(text.ends_with("suffisament long pour changer de format - "));

            size_t value{};
            functionCall->decodeParameter(value, undecodedData);
            require(value == text.size());
            require(undecodedData.empty());
        });
    }};

const Scenario functionReturnScenario{
    "FunctionReturn",
    [] {
        using Net = networking::NetworkingMock;

        given("A FunctionReturn message", [&] {
            msg::FunctionReturn<>              message;
            websocket::Frame<Net>              frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
            networking::SocketMock             socket;
            webfront::testing::InlineTaskScope scope;
            auto                               ws = websocket::WebSocket<Net>::create(socket, scope);

            then("Header command byte is functionReturn", [&] {
                require(static_cast<msg::Command>(message.header()[0]) == msg::Command::functionReturn);
            });

            when("An exception is encoded", [&] {
                std::string exceptionText = "Parameter error";
                auto        exception     = std::runtime_error(exceptionText);
                message.encodeParameter(exception, frame);
                ws->write(std::move(frame));

                then("Encoded frame should be", [&] {
                    auto encodedFrame = span(socket.debugBuffer.data(), socket.bufferIndex);
                    require(encodedFrame.size() == 2 + message.header().size() + 4 + bridge_code::generic.size() + exceptionText.size());
                });

                then("A Frame decoded should retrieve the encoded parameters", [&] {
                    websocket::FrameDecoder decoder;
                    require(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));

                    auto funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                    require(funcRet->getParametersCount() == 1);
                    require(funcRet->getPayloadSize() == 4 + bridge_code::generic.size() + exceptionText.size());

                    std::string text;
                    auto        undecodedData = funcRet->payload();
                    funcRet->decodeParameter(text, undecodedData);
                    require(text == exceptionText);
                    require(undecodedData.empty());
                });
            });

            when("A tuple is encoded", [&] {
                std::tuple<int, std::string> value{42, "Hello World"};
                message.encodeParameter(value, frame);
                ws->write(std::move(frame));

                then("An erroneous tuple should trigger an exception", [&] {
                    websocket::FrameDecoder decoder;
                    require(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));
                    auto                              funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                    std::tuple<int, std::string, int> tupleValue;
                    auto                              undecodedData = funcRet->payload();
                    requireThrows<std::runtime_error>([&] {
                        funcRet->decodeParameter(tupleValue, undecodedData);
                    });
                });
                then("A Frame decoded should retrieve the encoded parameters", [&] {
                    websocket::FrameDecoder decoder;
                    require(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));
                    auto funcRet = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                    require(funcRet->getParametersCount() == 1);
                    require(funcRet->getPayloadSize() == 24);

                    std::tuple<int, std::string> tupleValue;
                    auto                         undecodedData = funcRet->payload();
                    funcRet->decodeParameter(tupleValue, undecodedData);
                    require(std::get<0>(tupleValue) == 42);
                    require(std::get<1>(tupleValue) == "Hello World");
                    require(undecodedData.empty());
                });
            });
        });
    }};

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

const Scenario strings65536{"Strings of 65536 bytes or more use a 32-bit length", [] {
                                given("strings on both sides of the 16-bit boundary", [&] {
                                    then("255 bytes is a smallString, 256 to 65535 a string and 65536 a longString", [&] {
                                        require(firstCodeOf(string(255, 'x')) == msg::CodedType::smallString);
                                        require(firstCodeOf(string(256, 'x')) == msg::CodedType::string);
                                        require(firstCodeOf(string(65535, 'x')) == msg::CodedType::string);
                                        require(firstCodeOf(string(65536, 'x')) == msg::CodedType::longString);
                                    });
                                    then("each crosses the wire whole", [&] {
                                        for (const auto size : {255uz, 256uz, 65535uz, 65536uz, 65537uz, 1000000uz}) {
                                            const string text(size, 'x');
                                            require(roundTrip(text) == text);
                                        }
                                    });
                                });
                                given("a UTF-8 character that straddles the 65535-byte boundary", [&] {
                                    const string text = string(65534, 'x') + "\xc3\xa9" + string(10, 'y');  // 'é' occupies bytes 65534 and 65535
                                    then("it is not cut: the whole string is a longString", [&] {
                                        require(text.size() == 65546);
                                        require(firstCodeOf(text) == msg::CodedType::longString);
                                        require(roundTrip(text) == text);
                                    });
                                });
                                given("a string whose multi-byte characters take it past 65535 bytes", [&] {
                                    string text;
                                    for (int i = 0; i < 40000; ++i)
                                        text += "\xe2\x82\xac";  // the euro sign, 3 bytes
                                    then("the size counts bytes, not characters", [&] {
                                        require(roundTrip(text) == text);
                                    });
                                });
                                given("a longString header whose payload is truncated or inconsistent", [&] {
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
                                    then("a header shorter than five bytes is refused", [&] {
                                        const vector<byte> header{byte{static_cast<uint8_t>(msg::CodedType::longString)}, byte{1}, byte{0}};
                                        requireThrows<runtime_error>([&] {
                                            decode(header);
                                        });
                                    });
                                    then("a length that exceeds the bytes present is refused", [&] {
                                        requireThrows<runtime_error>([&] {
                                            decode(long5(70000, 69999));
                                        });
                                        requireThrows<runtime_error>([&] {
                                            decode(long5(0xffffffffu, 10));
                                        });
                                    });
                                    then("the exact length decodes and leaves the cursor after it", [&] {
                                        auto             bytes = long5(70000, 70000);
                                        string           target;
                                        span<const byte> payload{bytes};
                                        msg::FunctionCall<>::decodeParameter(target, payload);
                                        require(target.size() == 70000);
                                        require(payload.empty());
                                    });
                                    then("a destination that is not a string is refused", [&] {
                                        auto             bytes = long5(5, 5);
                                        int              number{};
                                        span<const byte> payload{bytes};
                                        requireThrows<runtime_error>([&] {
                                            msg::FunctionCall<>::decodeParameter(number, payload);
                                        });
                                    });
                                });
                                given("a policy whose messages cannot exceed 1 MiB", [&] {
                                    using SmallPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 1024 * 1024>;
                                    msg::FunctionReturn<SmallPolicy>             message;
                                    websocket::Frame<networking::NetworkingMock> frame{
                                        span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
                                    then("a string that fits is encoded", [&] {
                                        message.encodeParameter(string(1024 * 1024 - 8 - 5, 'x'), frame);
                                        require(message.getPayloadSize() == 1024 * 1024 - 8);
                                    });
                                    then("a string one byte too large throws length_error and leaves the message untouched", [&] {
                                        requireThrows<length_error>([&] {
                                            message.encodeParameter(string(1024 * 1024 - 8 - 4, 'x'), frame);
                                        });
                                        require(message.getPayloadSize() == 0);
                                        require(message.getParametersCount() == 0);
                                        require(frame.size() == message.header().size());
                                    });
                                    then("the bound counts what the message already carries", [&] {
                                        message.encodeParameter(string(600 * 1024, 'x'), frame);
                                        requireThrows<length_error>([&] {
                                            message.encodeParameter(string(600 * 1024, 'y'), frame);
                                        });
                                    });
                                });
                                given("an exception message longer than 65535 bytes, with a two-byte character across the limit", [&] {
                                    const runtime_error error{string(65534, 'x') + "\xc3\xa9" + string(10, 'y')};
                                    then("the message is cut before the first character that does not fit whole", [&] {
                                        require(roundTrip(error) == string(65534, 'x'));
                                    });
                                });
                            }};

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

const Scenario listsStrings{
    "Lists of strings, tuples and arrays are encoded as one list value",
    [] {
        given("an empty list of strings", [&] {
            const vector<string> empty;
            then("it is the list code and a zero count", [&] {
                require(encodedParameter(empty) == listBytes(0, {}));
                require(listRoundTrip(empty).empty());
            });
        });
        given("a list with one element, then several of different lengths", [&] {
            const vector<string> one{"alpha"};
            const vector<string> several{"", "beta", string(300, 'g'), string(70000, 'd')};
            then("elements keep their order and their own string header", [&] {
                require(encodedParameter(one) == listBytes(1, {wireString("alpha")}));
                require(listRoundTrip(one) == one);
                require(listRoundTrip(several) == several);
            });
            then("the list is one parameter, not one per element", [&] {
                msg::FunctionReturn<>                        message;
                websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
                message.encodeParameter(several, frame);
                require(message.getParametersCount() == 1);
            });
        });
        given("a list of records mixing strings, numbers, booleans and a Uint8Array", [&] {
            using Record = tuple<string, double, bool, vector<uint8_t>>;
            const vector<Record> entries{
                {"a.txt", 12.5, true, {1, 2, 3}},
                {"b", 0, false, {}},
                {"c", -1, true, vector<uint8_t>(300, 7)}
            };
            then("every field of every record crosses the wire", [&] {
                require(listRoundTrip(entries) == entries);
            });
        });
        given("a list of typed arrays and a list of bitsets", [&] {
            const vector<vector<uint8_t>> chunks{{1}, vector<uint8_t>(500, 9), {}};
            const vector<bitset<9>>       bits{bitset<9>{0x181}, bitset<9>{}};
            then("each is an element of its list", [&] {
                require(listRoundTrip(chunks) == chunks);
                require(listRoundTrip(bits) == bits);
            });
        });
        given("a tuple that holds a list, as a page of results with its cursor does", [&] {
            const tuple<vector<string>, string> page{
                {"x", "y"},
                "next"
            };
            then("the list is nested in the tuple, not in a list", [&] {
                require(listRoundTrip(page) == page);
            });
        });
        given("a list sent by JavaScript as a plain array, hence a tuple", [&] {
            vector<byte> tupleBytes{byte{static_cast<uint8_t>(msg::CodedType::tuple)}, byte{2}};
            for (const auto& element : {wireString("one"), wireString("two")})
                tupleBytes.insert(tupleBytes.end(), element.begin(), element.end());
            then("it decodes into a vector of the same elements", [&] {
                require(decodedParameter<vector<string>>(tupleBytes) == vector<string>{"one", "two"});
            });
            then("a tuple of other elements is still refused", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(
                        vector<byte>{byte{static_cast<uint8_t>(msg::CodedType::tuple)}, byte{1}, wireNumber(1)[0], wireNumber(1)[1]});
                });
            });
        });
        given("lists that are heterogeneous, truncated or inconsistent", [&] {
            then("an element of another type than the first is refused", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(listBytes(2, {wireString("a"), wireNumber(1)}));
                });
                requireThrows<runtime_error>([&] {
                    (decodedParameter<vector<tuple<string, int>>>(listBytes(2, {encodedParameter(tuple<string, int>{"a", 1}), wireString("b")})));
                });
            });
            then("a tuple of another arity than the first is refused", [&] {
                requireThrows<runtime_error>([&] {
                    (decodedParameter<vector<tuple<string, int>>>(
                        listBytes(2, {encodedParameter(tuple<string, int>{"a", 1}), encodedParameter(tuple<string>{"b"})})));
                });
            });
            then("a list or an exception inside a list is refused", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(listBytes(1, {listBytes(0, {})}));
                });
                const vector<byte> exception{byte{static_cast<uint8_t>(msg::CodedType::exception)}, byte{1}, byte{0}, byte{'x'}};
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(listBytes(1, {exception}));
                });
            });
            then("a count larger than the bytes that follow is refused without allocating for it", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(listBytes(0xffffffffu, {wireString("a")}));
                });
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(listBytes(3, {wireString("a"), wireString("b")}));
                });
            });
            then("a header shorter than five bytes, or an element cut short, is refused", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(vector<byte>{byte{static_cast<uint8_t>(msg::CodedType::list)}, byte{1}});
                });
                auto cut = listBytes(1, {wireString("abcdef")});
                cut.pop_back();
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(cut);
                });
            });
            then("a list decoded into another kind of destination is refused", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<string>(listBytes(0, {}));
                });
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<int>>(listBytes(0, {}));
                });
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<string>>(encodedParameter(vector<uint8_t>{1}));
                });
            });
        });
        given("elements whose values decode into the destination but whose wire shapes differ", [&] {
            const auto boolTuple   = encodedParameter(tuple<bool>{true});
            const auto numberTuple = encodedParameter(tuple<double>{1});
            then("a list of [true] and [1] is refused although a number decodes as a bool", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<tuple<bool>>>(listBytes(2, {boolTuple, numberTuple}));
                });
            });
            then("the same holds for a plain JavaScript array, which arrives as a tuple", [&] {
                vector<byte> asTuple{byte{static_cast<uint8_t>(msg::CodedType::tuple)}, byte{2}};
                for (const auto* element : {&boolTuple, &numberTuple})
                    asTuple.insert(asTuple.end(), element->begin(), element->end());
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<tuple<bool>>>(asTuple);
                });
            });
            then("true and false are one shape", [&] {
                require(decodedParameter<vector<tuple<bool>>>(listBytes(2, {boolTuple, encodedParameter(tuple<bool>{false})})).size() == 2);
            });
            then("different fields at the same position are refused, equal shapes with different string headers are accepted", [&] {
                requireThrows<runtime_error>([&] {
                    (decodedParameter<vector<tuple<string, double>>>(
                        listBytes(2, {encodedParameter(tuple<string, double>{"a", 1}), encodedParameter(tuple<double, string>{2, "b"})})));
                });
                const vector<tuple<string, double>> mixed{
                    {"a", 1},
                    {string(300, 'b'), 2},
                    {string(70000, 'c'), 3}
                };
                require(listRoundTrip(mixed) == mixed);
            });
            then("the short and the regular Uint8Array are one shape, other element types are not", [&] {
                const vector<vector<uint8_t>> bytes{vector<uint8_t>(3, 1), vector<uint8_t>(300, 2)};
                require(listRoundTrip(bytes) == bytes);
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<vector<uint8_t>>>(listBytes(2, {encodedParameter(vector<uint8_t>{1}), encodedParameter(vector<int8_t>{1})}));
                });
            });
        });
        given("tuples nested far deeper than any application type", [&] {
            // A list whose first element is 100000 tuples of one field each: 2 bytes of header per level.
            vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::list)}, byte{1}, byte{0}, byte{0}, byte{0}};
            for (int level = 0; level < 100000; ++level) {
                bytes.push_back(byte{static_cast<uint8_t>(msg::CodedType::tuple)});
                bytes.push_back(byte{1});
            }
            bytes.push_back(byte{static_cast<uint8_t>(msg::CodedType::booleanTrue)});
            then("the shape check refuses them before the stack is at risk", [&] {
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<tuple<bool>>>(bytes);
                });
            });
        });
        given("tuples nested exactly at and just past the shared limit of 32 levels", [&] {
            const auto nested = [](int levels) {
                vector<byte> bytes{byte{static_cast<uint8_t>(msg::CodedType::list)}, byte{1}, byte{0}, byte{0}, byte{0}};
                for (int level = 0; level < levels; ++level) {
                    bytes.push_back(byte{static_cast<uint8_t>(msg::CodedType::tuple)});
                    bytes.push_back(byte{1});
                }
                bytes.push_back(byte{static_cast<uint8_t>(msg::CodedType::booleanTrue)});
                return bytes;
            };
            // The destination only has to be as deep as the wire value for decoding to reach the check: a 33-level destination
            // type is refused at compile time by the encoder, so the wire is built by hand.
            then("32 levels pass the shape check and 33 are refused", [&] {
                using Level32 = tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<
                    tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<tuple<bool>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>;
                static_assert(msg::tuple_depth_v<Level32> == 32);
                requireNothrow([&] {
                    decodedParameter<vector<Level32>>(nested(32));
                });
                requireThrows<runtime_error>([&] {
                    decodedParameter<vector<Level32>>(nested(33));
                });
            });
        });
    }};

// A scenario of its own: each message and frame holds its policy's buffers, and one function with all of them exceeds
// the 64 KiB of stack that the MSVC analysis allows (C6262, #512).
const Scenario listsRespect{
    "Lists respect the bounds of the buffers policy",
    [] {
        given("a policy whose messages cannot exceed 1 KiB", [&] {
            using SmallPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 1024>;
            msg::FunctionReturn<SmallPolicy>             message;
            websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
            then("a list that does not fit throws length_error", [&] {
                requireThrows<length_error>([&] {
                    message.encodeParameter(vector<string>(200, string(10, 'x')), frame);
                });
            });
        });
        given("a policy whose header buffer is small", [&] {
            using TinyPolicy = http::CustomBuffersPolicy<8192, 8192, 64>;
            msg::FunctionReturn<TinyPolicy>              message;
            websocket::Frame<networking::NetworkingMock> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
            then("a list with more element headers than the buffer holds overflows explicitly", [&] {
                requireThrows<http::BufferOverflowException>([&] {
                    message.encodeParameter(vector<string>(100, "x"), frame);
                });
            });
        });
    }};

const Scenario typedArray{"A typed array that exactly fills the message size is encoded", [] {
                              using ExactPolicy = http::CustomBuffersPolicy<8192, 8192, 32768, 32>;
                              msg::FunctionReturn<ExactPolicy>             message;
                              websocket::Frame<networking::NetworkingMock> frame{
                                  span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
                              then("its header is counted once", [&] {
                                  const vector<uint8_t> bytes(22, 1);  // 8 header bytes + 2 + 22 = 32
                                  requireNothrow([&] {
                                      message.encodeParameter(bytes, frame);
                                  });
                                  require(message.getPayloadSize() == 24);
                              });
                              then("one more byte is refused", [&] {
                                  requireThrows<length_error>([&] {
                                      message.encodeParameter(vector<uint8_t>(23, 1), frame);
                                  });
                              });
                          }};

const Scenario exceptionsCarry{"Exceptions carry a code and a message", [] {
                                   auto decodeBoth = [](const vector<byte>& bytes) {
                                       string           code, message;
                                       span<const byte> payload{bytes};
                                       msg::FunctionReturn<>::decodeException(code, message, payload);
                                       require(payload.empty());
                                       return pair<string, string>{code, message};
                                   };
                                   given("an application error", [&] {
                                       const BridgeError error{"quota:exceeded", "no space left"};
                                       then("the wire carries the code, then the message", [&] {
                                           const auto bytes = encodedParameter(error);
                                           require(bytes.size() == 2 + 14 + 2 + 13);
                                           require(bytes[0] == byte{static_cast<uint8_t>(msg::CodedType::exception)});
                                           require(bytes[1] == byte{14});
                                           require(decodeBoth(bytes) == pair<string, string>{"quota:exceeded", "no space left"});
                                       });
                                       then("decoded as a string, the exception is its message", [&] {
                                           require(roundTrip(error) == "no space left");
                                       });
                                   });
                                   given("an ordinary exception", [&] {
                                       then("it is sent with the generic code", [&] {
                                           require(decodeBoth(encodedParameter(runtime_error{"boom"}))
                                                   == pair<string, string>{string(bridge_code::generic), "boom"});
                                           require(decodeBoth(encodedParameter(out_of_range{"gone"})).first == bridge_code::generic);
                                       });
                                   });
                                   given("an error of the bridge", [&] {
                                       then("its reserved code crosses the wire", [&] {
                                           require(decodeBoth(encodedParameter(detail::makeBridgeError(bridge_code::cancelled, "stopped"))).first
                                                   == bridge_code::cancelled);
                                       });
                                   });
                                   given("an exception message longer than 65535 bytes", [&] {
                                       then("the code is kept and the message is cut before a character that does not fit", [&] {
                                           const BridgeError error{"big", string(65534, 'x') + "\xc3\xa9" + string(10, 'y')};
                                           require(decodeBoth(encodedParameter(error)) == pair<string, string>{"big", string(65534, 'x')});
                                       });
                                   });
                                   given("a truncated exception", [&] {
                                       auto bytes = encodedParameter(BridgeError{"code", "message"});
                                       then("every cut is refused", [&] {
                                           for (size_t keep = 0; keep < bytes.size(); ++keep) {
                                               const vector<byte> cut(bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(keep));
                                               string             code, message;
                                               span<const byte>   payload{cut};
                                               requireThrows<runtime_error>([&] {
                                                   msg::FunctionReturn<>::decodeException(code, message, payload);
                                               });
                                           }
                                       });
                                   });
                                   given("a code that is ill-formed or reserved", [&] {
                                       then("an application cannot build the error", [&] {
                                           requireThrows<invalid_argument>([&] {
                                               BridgeError("", "m");
                                           });
                                           requireThrows<invalid_argument>([&] {
                                               BridgeError(string(65, 'a'), "m");
                                           });
                                           requireThrows<invalid_argument>([&] {
                                               BridgeError("with space", "m");
                                           });
                                           requireThrows<invalid_argument>([&] {
                                               BridgeError("webfront.cancelled", "m");
                                           });
                                           requireNothrow([&] {
                                               BridgeError(string(64, 'a'), "m");
                                           });
                                       });
                                       then("a peer's ill-formed code becomes the generic one", [&] {
                                           require(detail::makeBridgeError("bad code", "m").code() == bridge_code::generic);
                                       });
                                   });
                               }};

const Scenario functionCallCorrelation{"Function call correlation identifiers", [] {
                                           msg::FunctionCall<> message;
                                           message.setCallId(42);
                                           require(message.getCallId() == 42);
                                           message.reset();
                                           require(message.getCallId() == 0);
                                       }};

const Scenario numericArrays{"Numeric arrays are encoded and decoded as owning values", [] {
                                 using Net = networking::NetworkingMock;

                                 auto roundTrip = []<typename Element>(const vector<Element>& input) {
                                     msg::FunctionReturn<>  message;
                                     websocket::Frame<Net>  frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
                                     networking::SocketMock socket;
                                     webfront::testing::InlineTaskScope scope;
                                     auto                               ws = websocket::WebSocket<Net>::create(socket, scope);

                                     message.encodeParameter(input, frame);
                                     ws->write(std::move(frame));

                                     websocket::FrameDecoder decoder;
                                     require(decoder.parse(span(socket.debugBuffer.data(), socket.bufferIndex)));
                                     const auto*     decodedMessage = msg::FunctionReturn<>::castFromRawData(decoder.payload());
                                     auto            payload        = decodedMessage->payload();
                                     vector<Element> output;
                                     decodedMessage->decodeParameter(output, payload);
                                     require(output == input);
                                     require(payload.empty());
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
                             }};

const Scenario uint8Arrays{"Uint8 arrays switch encoding at the compact-length boundary", [] {
                               using Net = networking::NetworkingMock;

                               auto encodedType = []<typename Array>(const Array& input) {
                                   msg::FunctionReturn<> message;
                                   websocket::Frame<Net> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
                                   message.encodeParameter(input, frame);
                                   const auto buffers = frame.toBuffers();
                                   require(buffers.size() == 4);
                                   const auto type = *reinterpret_cast<const byte*>(buffers[2].data());
                                   return static_cast<msg::CodedType>(to_integer<uint8_t>(type));
                               };

                               vector<uint8_t>     compact(255, 7);
                               vector<uint8_t>     regular(256, 9);
                               array<uint8_t, 255> fixedCompact{};
                               array<uint8_t, 256> fixedRegular{};

                               require(encodedType(compact) == msg::CodedType::smallArrayU8);
                               require(encodedType(regular) == msg::CodedType::arrayU8);
                               require(encodedType(fixedCompact) == msg::CodedType::smallArrayU8);
                               require(encodedType(fixedRegular) == msg::CodedType::arrayU8);
                           }};

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

const Scenario numericArraysDecode{"Numeric arrays decode into fixed-size destinations", [] {
                                       array<uint16_t, 3> output{};
                                       span<const byte>   payload{threeUint16s};
                                       msg::FunctionCall<>::decodeParameter(output, payload);
                                       require(output == array<uint16_t, 3>{1, 2, 3});
                                       require(payload.empty());
                                   }};

const Scenario malformedNumeric{"Malformed numeric array payloads are rejected", [] {
                                    given("A payload shorter than its declared element count", [&] {
                                        vector<uint8_t>  output;
                                        array<byte, 3>   truncated{byte{static_cast<uint8_t>(msg::CodedType::smallArrayU8)}, byte{2}, byte{1}};
                                        span<const byte> payload{truncated};
                                        requireThrows<runtime_error>([&] {
                                            msg::FunctionCall<>::decodeParameter(output, payload);
                                        });
                                    });

                                    given("A fixed-size destination whose length disagrees with the payload", [&] {
                                        array<uint16_t, 2> fixed{};
                                        span<const byte>   payload{threeUint16s};
                                        requireThrows<runtime_error>([&] {
                                            msg::FunctionCall<>::decodeParameter(fixed, payload);
                                        });
                                    });

                                    given("A wire type that does not match the destination element type", [&] {
                                        vector<uint32_t> output;
                                        span<const byte> payload{threeUint16s};
                                        requireThrows<runtime_error>([&] {
                                            msg::FunctionCall<>::decodeParameter(output, payload);
                                        });
                                    });

                                    given("An array header truncated before its length field", [&] {
                                        vector<uint16_t> output;
                                        array<byte, 2>   shortHeader{byte{static_cast<uint8_t>(msg::CodedType::arrayU16)}, byte{1}};
                                        span<const byte> payload{shortHeader};
                                        requireThrows<runtime_error>([&] {
                                            msg::FunctionCall<>::decodeParameter(output, payload);
                                        });
                                    });
                                }};

const Scenario decodeParameterRejects{"decodeParameter rejects malformed and mistyped payloads", [] {
                                          auto decodeInto = []<typename T>(T& target, span<const byte> bytes) {
                                              span<const byte> payload{bytes};
                                              msg::FunctionCall<>::decodeParameter(target, payload);
                                          };
                                          auto coded = [](msg::CodedType type) {
                                              return byte{static_cast<uint8_t>(type)};
                                          };

                                          given("An empty payload", [&] {
                                              string           target;
                                              span<const byte> payload{};
                                              requireThrows<runtime_error>([&] {
                                                  msg::FunctionCall<>::decodeParameter(target, payload);
                                              });
                                          });

                                          given("An unknown coded type", [&] {
                                              string         target;
                                              array<byte, 1> unknown{byte{0xfe}};
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, unknown);
                                              });
                                          });

                                          given("A value decoded into the wrong destination type", [&] {
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

                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(asString, boolean);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(asBool, smallString);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(asString, number);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(asInt, tuple);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(asInt, array8);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(asVector, boolean);
                                              });
                                          });

                                          given("String payloads truncated after their type byte", [&] {
                                              string         target;
                                              array<byte, 1> smallHeader{coded(msg::CodedType::smallString)};
                                              array<byte, 2> longHeader{coded(msg::CodedType::string), byte{4}};
                                              array<byte, 3> smallBody{coded(msg::CodedType::smallString), byte{8}, byte{'a'}};
                                              array<byte, 4> longBody{coded(msg::CodedType::string), byte{8}, byte{0}, byte{'a'}};

                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, smallHeader);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, longHeader);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, smallBody);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, longBody);
                                              });
                                          });

                                          given("A number payload shorter than a double", [&] {
                                              double         target{};
                                              array<byte, 4> shortNumber{coded(msg::CodedType::number), byte{0}, byte{0}, byte{0}};
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, shortNumber);
                                              });
                                          });

                                          given("A tuple whose declared arity differs from the destination", [&] {
                                              tuple<int, string> target;
                                              array<byte, 2>     wrongArity{coded(msg::CodedType::tuple), byte{3}};
                                              array<byte, 1>     truncatedHeader{coded(msg::CodedType::tuple)};
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, wrongArity);
                                              });
                                              requireThrows<runtime_error>([&] {
                                                  decodeInto(target, truncatedHeader);
                                              });
                                          });
                                      }};

const Scenario bufferOverflow{"BufferOverflow", [] {
                                  using Net        = networking::NetworkingMock;
                                  using TinyPolicy = http::CustomBuffersPolicy<8192, 8192, 12>;

                                  given("A FunctionCall with a tiny buffer", [&] {
                                      msg::FunctionCall<TinyPolicy> message;
                                      websocket::Frame<Net>         frame{
                                          std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};

                                      when("Number overflows the buffer", [&] {
                                          message.encodeParameter("fn", frame);  // 2 bytes in buffer (type + uint8 size)
                                          message.encodeParameter(1, frame);     // 9 bytes in buffer (type + double) → total 11

                                          then("Next number overflows", [&] {
                                              requireThrows<http::BufferOverflowException>([&] {
                                                  message.encodeParameter(2, frame);
                                              });
                                          });
                                      });

                                      when("Boolean overflows the buffer", [&] {
                                          message.encodeParameter("fn", frame);  // 2 bytes → total 2
                                          message.encodeParameter(1, frame);     // 9 bytes → total 11
                                          message.encodeParameter(true, frame);  // 1 byte  → total 12 (exactly full)

                                          then("Next boolean overflows", [&] {
                                              requireThrows<http::BufferOverflowException>([&] {
                                                  message.encodeParameter(false, frame);
                                              });
                                          });
                                      });

                                      when("String header overflows the buffer", [&] {
                                          message.encodeParameter("fn", frame);  // 2 bytes → total 2
                                          message.encodeParameter(1, frame);     // 9 bytes → total 11

                                          then("Next string header overflows", [&] {
                                              requireThrows<http::BufferOverflowException>([&] {
                                                  message.encodeParameter("x", frame);
                                              });
                                          });
                                      });
                                  });
                              }};

const Scenario emptyWire{"Empty wire strings are decoded without indexing past the input", [] {
                             for (auto bytes : {
                                      std::vector<std::byte>{std::byte{4}, std::byte{0}},
                                      std::vector<std::byte>{std::byte{5}, std::byte{0}, std::byte{0}}
                             }) {
                                 std::span<const std::byte> input(bytes);
                                 std::string                value = "previous";
                                 msg::FunctionCall<>::decodeParameter(value, input);
                                 require(value.empty());
                                 require(input.empty());
                             }
                         }};

const Scenario wireNumbers{"Wire numbers outside an integer result range are rejected", [] {
                               for (double number :
                                    {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 2147483648.0, -2147483649.0}) {
                                   std::array<std::byte, 9> bytes{std::byte{3}};
                                   std::memcpy(bytes.data() + 1, &number, sizeof(number));
                                   std::span<const std::byte> input(bytes);
                                   int                        value{};
                                   requireThrows<std::runtime_error>([&] {
                                       msg::FunctionCall<>::decodeParameter(value, input);
                                   });
                               }
                           }};

const Scenario wireInteger{"Wire integer conversion keeps truncation and handles 64-bit boundaries", [] {
                               auto decode = []<typename T>(double number, T& value) {
                                   std::array<std::byte, 9> bytes{std::byte{3}};
                                   std::memcpy(bytes.data() + 1, &number, sizeof(number));
                                   std::span<const std::byte> input(bytes);
                                   msg::FunctionCall<>::decodeParameter(value, input);
                                   require(input.empty());
                               };
                               int value{};
                               decode(42.75, value);
                               require(value == 42);
                               decode(-42.75, value);
                               require(value == -42);
                               decode(2147483647.75, value);
                               require(value == std::numeric_limits<int>::max());

                               std::uint64_t unsignedValue{};
                               requireThrows<std::runtime_error>([&] {
                                   decode(18446744073709551616.0, unsignedValue);
                               });
                               requireThrows<std::runtime_error>([&] {
                                   decode(-1.0, unsignedValue);
                               });
                               decode(9223372036854775808.0, unsignedValue);
                               require(unsignedValue == (std::uint64_t{1} << 63));
                               std::int64_t signedValue{};
                               requireThrows<std::runtime_error>([&] {
                                   decode(9223372036854775808.0, signedValue);
                               });
                               decode(-9223372036854775808.0, signedValue);
                               require(signedValue == std::numeric_limits<std::int64_t>::min());
                           }};

namespace {
template <typename... Values>
vector<byte> bitsetPayload(const Values&... values) {
    msg::FunctionReturn<>                        message;
    websocket::Frame<networking::NetworkingMock> frame{message.header()};
    (message.encodeParameter(values, frame), ...);
    require(message.getParametersCount() == sizeof...(Values));
    vector<byte> payload;
    const auto   buffers = frame.toBuffers();
    for (size_t i = 2; i < buffers.size(); ++i) {
        const auto* start = static_cast<const byte*>(buffers[i].data());
        if (buffers[i].size())
            payload.insert(payload.end(), start, start + buffers[i].size());
    }
    require(payload.size() == message.getPayloadSize());
    return payload;
}

template <size_t N>
void expectPackedBitsetBytes(span<const byte> bytes, const bitset<N>& input) {
    constexpr size_t count  = N / 8 + (N % 8 != 0);
    constexpr size_t header = count < 256 ? 2 : 5;
    if constexpr (N != 0)
        for (size_t b = 0; b < count; ++b) {
            unsigned expected = 0;
            for (size_t bit = 0; bit < 8 && b * 8 + bit < N; ++bit)
                if (input[b * 8 + bit])
                    expected |= 1u << bit;
            require(to_integer<unsigned>(bytes[header + b]) == expected);
        }
}
}  // namespace

const Scenario bitsetsUse{"Bitsets use canonical packed Uint8Array bytes at every size boundary", [] {
                              auto check = []<size_t N>() {
                                  bitset<N> input;
                                  if constexpr (N != 0)
                                      for (size_t i = 0; i < N; ++i)
                                          input[i] = i % 3 == 0 || i + 1 == N;
                                  const auto       bytes  = bitsetPayload(input);
                                  constexpr size_t count  = N / 8 + (N % 8 != 0);
                                  constexpr size_t header = count < 256 ? 2 : 5;
                                  require(bytes.size() == header + count);
                                  require(bytes[0] == byte(count < 256 ? msg::CodedType::smallArrayU8 : msg::CodedType::arrayU8));
                                  uint32_t length{};
                                  if constexpr (count < 256)
                                      length = to_integer<uint8_t>(bytes[1]);
                                  else
                                      memcpy(&length, bytes.data() + 1, sizeof(length));
                                  require(length == count);
                                  expectPackedBitsetBytes(span<const byte>{bytes}, input);
                                  span<const byte> data{bytes};
                                  bitset<N>        output;
                                  output.set();
                                  msg::FunctionReturn<>::decodeParameter(output, data);
                                  // Boolean assertions avoid MSVC STL stream diagnostics for bitset<0>.
                                  require((output == input));
                                  require(data.empty());
                                  // All-one and all-zero patterns also exercise the partial last byte.
                                  input.set();
                                  const auto ones = bitsetPayload(input);
                                  data            = ones;
                                  msg::FunctionReturn<>::decodeParameter(output, data);
                                  require((output == input));
                                  input.reset();
                                  const auto zeros = bitsetPayload(input);
                                  data             = zeros;
                                  msg::FunctionReturn<>::decodeParameter(output, data);
                                  require(output.none());
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
                              require(bitsetPayload(bitset<9>{0x181}) == vector<byte>{byte(msg::CodedType::smallArrayU8), byte{2}, byte{0x81}, byte{1}});
                          }};

const Scenario packedBitsetsScenario{"Packed bitsets retain owned bytes, tuple counts and the decoder cursor", [] {
                                         const auto expected = tuple{
                                             bitset<9>{0x181},
                                             tuple{bitset<0>{}, bitset<65>{9}},
                                             string("tail")
                                         };
                                         const auto                         bytes = bitsetPayload(expected, bitset<8>{0xa5}, 42);
                                         span<const byte>                   data{bytes};
                                         remove_cvref_t<decltype(expected)> output{};
                                         msg::FunctionReturn<>::decodeParameter(output, data);
                                         require((output == expected));
                                         bitset<8> bits;
                                         msg::FunctionReturn<>::decodeParameter(bits, data);
                                         require(bits.to_ulong() == 0xa5);
                                         int tail{};
                                         msg::FunctionReturn<>::decodeParameter(tail, data);
                                         require(tail == 42);
                                         require(data.empty());
                                     }};

const Scenario bitsetDecoding{"Bitset decoding rejects wrong types, lengths, padding and truncation", [] {
                                  const auto valid  = bitsetPayload(bitset<9>{0x181});
                                  auto       reject = [](const vector<byte>& bytes) {
                                      span<const byte> data{bytes};
                                      bitset<9>        output{7};
                                      requireThrows<runtime_error>([&] {
                                          msg::FunctionReturn<>::decodeParameter(output, data);
                                      });
                                      require(output.to_ulong() == 7);
                                      require(data.size() == bytes.size());
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
                                  require(output.to_ulong() == 0x181);
                                  require(data.empty());
                              }};

const Scenario encodingBridge{
    "Encoding bridge messages writes nothing to the standard output",
    [] {
        given("the standard output captured while a call and a return are encoded with every scalar kind", [&] {
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
            then("the encoder printed nothing (the HTML bridge paid for console output on every message)", [&] {
                require(captured.str().empty());
            });
        });
    }};
