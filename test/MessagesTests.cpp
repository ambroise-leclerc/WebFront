#include <weblink/Messages.hpp>

#include <catch2/catch.hpp>

using namespace webfront;
SCENARIO("Handshake message") {
    std::array<uint8_t, 2> raw{0x00, 0x00};
    auto handshake = msg::Handshake::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));

    REQUIRE(sizeof(*handshake) == 2);
    REQUIRE(handshake->getEndian() == msg::JSEndian::little);
}

SCENARIO("Ack message") {
    msg::Ack ack;

    REQUIRE(sizeof(ack) == 2);
}

SCENARIO("FunctionCall") {
    GIVEN("Raw data of a call to 'print' function with a string as parameter") {
    std::array<uint8_t, 36> raw{
      0x03, 0x02, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x04, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x04, 0x13, 0x48,
      0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x20, 0x32, 0x30, 0x32, 0x32};
      
    auto functionCall  = msg::FunctionCall::castFromRawData(std::span(reinterpret_cast<const std::byte*>(raw.data()), raw.size()));
    REQUIRE(functionCall->getParametersCount() == 2);
    REQUIRE(functionCall->getPayloadSize() == 28);
    auto [name, undecodedData] = functionCall->getFunctionName();
    REQUIRE(name == "print");
    std::string text;
    functionCall->decodeParameter(text, undecodedData);
    REQUIRE(text == "Hello World of 2022");
    REQUIRE(undecodedData.size() == 0);
    }
}
