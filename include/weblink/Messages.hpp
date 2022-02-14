/// @date 13/02/2022 21:24:42
/// @author Ambroise Leclerc
/// @brief Messages exchanged between webfront clients and server
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>

namespace webfront::msg {

enum class JSEndian : uint8_t { little = 0, big = 1, mixed = little + big };
enum class TxtOpcode : uint8_t { debugLog, injectScript };

enum class Command : uint8_t {
    handshake,
    ack,
    textCommand,
    callFunction,   // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
    functionReturn, // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
};


enum class CodedType : uint8_t {
    undefined,
    booleanTrue,  // opcode if boolean is true
    booleanFalse, // opcode if boolean is false
    number,       // opcode + 8 bytes IEEE754 floating point number
    smallString,  // opcode + 1 byte size
    string,       // opcode + 2 bytes size
};

inline std::string_view toString(CodedType t) {
    switch (t) {
    case CodedType::booleanTrue: return "boolean (true)";
    case CodedType::booleanFalse: return "boolean (false)";
    case CodedType::number: return "number";
    case CodedType::smallString: return "smallString";
    case CodedType::string: return "string";
    default: return "undefined";
    }
}


template<typename T>
class MessageBase {
public:
    [[nodiscard]] std::span<const std::byte> header() const { return {reinterpret_cast<const std::byte*>(this), sizeof(typename T::Header)}; }
    [[nodiscard]] std::span<const std::byte> payload() const {
        return {header().data() + sizeof(typename T::Header), static_cast<const T*>(this)->getPayloadSize()};
    }
    [[nodiscard]] size_t getPayloadSize() const { return 0; };

    static const T* castFromRawData(std::span<const std::byte> data) {
        if (data.size() < sizeof(typename T::Header)) throw std::runtime_error("Not enough data to form a message Header");
        auto message = reinterpret_cast<const T*>(data.data());
        if ((data.size() + message->getPayloadSize()) < sizeof(typename T::Header)) throw std::runtime_error("Not enough data to form a complete message");
        return message;
    }
};

// First message sent by JS client to WebFront
class Handshake : public MessageBase<Handshake> {
    struct Header {
        Command command = Command::handshake;
        JSEndian endian = {};
    } head;
    static_assert(sizeof(Header) == 2, "Handshake header has to be 2 bytes long");
    friend class MessageBase<Handshake>;

public:
    [[nodiscard]] JSEndian getEndian() const { return head.endian; }
};

// Message sent by WebFront to acknoledge a Handshake message
class Ack : public MessageBase<Ack> {
    struct Header {
        Command command = Command::ack;
        JSEndian endian = std::endian::native == std::endian::little ? JSEndian::little : JSEndian::big;
    } head;
    static_assert(sizeof(Header) == 2, "Ack header has to be 2 bytes long");
    friend class MessageBase<Ack>;
};

/// Commands with a text payload
class TextCommand : public MessageBase<TextCommand> {
    struct Header {
        Command command = Command::textCommand;
        TxtOpcode opcode;
        uint8_t lengthHi, lengthLo; // Payload length = (256 * lengthHi) + lengthLo
    } head;
    static_assert(sizeof(Header) == 4, "TextCommand header has to be 4 bytes long");

    std::span<const std::byte> payloadSpan;
    friend class MessageBase<TextCommand>;

public:
    TextCommand(TxtOpcode opcode, std::string_view text)
        : head{.opcode = opcode, .lengthHi = static_cast<uint8_t>(text.size() >> 8), .lengthLo = static_cast<uint8_t>(text.size())},
          payloadSpan{reinterpret_cast<const std::byte*>(text.data()), text.size()} {}

    [[nodiscard]] std::span<const std::byte> payload() const { return payloadSpan; }
    [[nodiscard]] size_t getPayloadSize() const { return static_cast<uint16_t>(256 * head.lengthHi + head.lengthLo); }
};

/// Encodes a list of parameters : parameters 0 is the function. Sent either by WebFront or by the JS Client
class FunctionCall : public MessageBase<FunctionCall> {
    struct Header {
        Command command = Command::callFunction;
        uint8_t parametersCount = 0;
        std::array<uint8_t, 2> padding{};
        uint32_t parametersDataSize = 0;
    } head;
    static_assert(sizeof(Header) == 8, "FunctionCall header has to be 8 bytes long");
    friend class MessageBase<FunctionCall>;

public:
    void setParametersCount(uint8_t parametersCount) { head.parametersCount = parametersCount; }
    void setPayloadSize(uint32_t size) { head.parametersDataSize = size; }
    [[nodiscard]] uint8_t getParametersCount() const { return head.parametersCount; }
    [[nodiscard]] size_t getPayloadSize() const { return head.parametersDataSize; }
    [[nodiscard]] std::tuple<std::string, std::span<const std::byte>> getFunctionName() const {
        std::string functionName;
        auto data = payload();
        decodeParameter(functionName, data);
        return {functionName, data};
    }

    template<typename T>
    static void decodeParameter(T& param, std::span<const std::byte>& data) {
        if (data.size() == 0) throw std::runtime_error("Not enough data for msg::FunctionCall::decodeParameter");
        auto codedType = static_cast<CodedType>(data[0]);
        switch (codedType) {
        case CodedType::smallString:
            if constexpr (std::is_same_v<T, std::string>) {
                if (data.size() < 1) throw std::runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                auto size = static_cast<size_t>(data[1]);
                if (data.size() < 2 + size) throw std::runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                param = std::string(reinterpret_cast<const char*>(&data[2]), size);
                data = data.subspan(2 + size);
            }
            break;
        default: param = {};
        }
    }
};

/// Encodes FunctionCall return values (or exceptions)
class FunctionReturn : public MessageBase<FunctionReturn> {
    struct Header {
        Command command = Command::functionReturn;
        uint8_t parametersCount = 0;
        std::array<uint8_t, 2> padding{};
        uint32_t parametersDataSize = 0;
    } head;
    static_assert(sizeof(Header) == 8, "FunctionReturn header has to be 8 bytes long");
    friend class MessageBase<FunctionReturn>;
};

} // namespace webfront::msg
