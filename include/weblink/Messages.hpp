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

#include <iostream>

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
    exception,    // opcode + 2 bytes size
    smallArrayU8, // opcode + 1 byte size,
    arrayU8,      // opcode + 4 bytes size,
    array8,       // opcode + 4 bytes size,
    arrayU16,     // opcode + 4 bytes size,
    array16,      // opcode + 4 bytes size,
    arrayU32,     // opcode + 4 bytes size,
    array32,      // opcode + 4 bytes size,
    arrayU64,     // opcode + 4 bytes size,
    array64,      // opcode + 4 bytes size,
    arrayFloat,   // opcode + 4 bytes size,
    arrayDouble,  // opcode + 4 bytes size,
};

inline std::string_view toString(CodedType t) {
    switch (t) {
    case CodedType::booleanTrue: return "boolean (true)";
    case CodedType::booleanFalse: return "boolean (false)";
    case CodedType::number: return "number";
    case CodedType::smallString: return "smallString";
    case CodedType::string: return "string";
    case CodedType::exception: return "exception";
    case CodedType::smallArrayU8: return "smallArrayU8";
    case CodedType::arrayU8: return "arrayU8";
    case CodedType::array8: return "array8";
    case CodedType::arrayU16: return "arrayU16";
    case CodedType::array16: return "array16";
    case CodedType::arrayU32: return "arrayU32";
    case CodedType::array32: return "array32";
    case CodedType::arrayU64: return "arrayU64";
    case CodedType::array64: return "array64";
    case CodedType::arrayFloat: return "arrayFloat";
    case CodedType::arrayDouble: return "arrayDouble";
    default: return "undefined";
    }
}

template<typename _Tp, typename dummy = void>
struct is_printable : std::false_type {};

template<typename T>
struct is_printable<T, std::void_t<decltype(std::cout << std::declval<T>())>> : std::true_type {};
        

template<typename T>
class MessageBase {
public:
    [[nodiscard]] std::span<const std::byte> header() const {
        return {reinterpret_cast<const std::byte*>(this), sizeof(typename T::Header)};
    }
    [[nodiscard]] std::span<const std::byte> payload() const {
        return {header().data() + sizeof(typename T::Header), static_cast<const T*>(this)->getPayloadSize()};
    }
    [[nodiscard]] size_t getPayloadSize() const { return 0; };

    static const T* castFromRawData(std::span<const std::byte> data) {
        if (data.size() < sizeof(typename T::Header)) throw std::runtime_error("Not enough data to form a message Header");
        auto message = reinterpret_cast<const T*>(data.data());
        if ((data.size() + message->getPayloadSize()) < sizeof(typename T::Header))
            throw std::runtime_error("Not enough data to form a complete message");
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
        : head{.opcode = opcode,
               .lengthHi = static_cast<uint8_t>(text.size() >> 8),
               .lengthLo = static_cast<uint8_t>(text.size())},
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

    std::array<std::byte, 1024>
      buffer; // When composing a message, buffer does not represent the payload but some bufferized data to be sent
    size_t encodeBufferIndex = 0;
    friend class MessageBase<FunctionCall>;

    template<typename T>
    constexpr auto typeName() {
#if defined(_MSC_VER)
        std::string_view tName = __FUNCSIG__, prefix = "auto __cdecl JSFunction::typeName<", suffix = ">(void)";
#elif __clang__
        std::string_view tName = __PRETTY_FUNCTION__, prefix = "auto webfront::JSFunction::typeName() [T = ", suffix = "]";
#elif defined(__GNUC__)
        std::string_view tName = __PRETTY_FUNCTION__,
                         prefix = "constexpr auto webfront::JSFunction::typeName() [with T = ", suffix = "]";
#endif
        tName.remove_prefix(prefix.size());
        tName.remove_suffix(suffix.size());
        return tName;
    }

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

    // types with a size
    template<typename T, typename WebSocketFrame>
    size_t encodeType(msg::CodedType type, T size, WebSocketFrame& frame) {
        frame.addBuffer(std::span(&buffer[encodeBufferIndex], 1 + sizeof(size)));
        buffer[encodeBufferIndex++] = static_cast<std::byte>(type);
        std::copy_n(reinterpret_cast<const std::byte*>(&size), sizeof(size), &buffer[encodeBufferIndex]);
        encodeBufferIndex += sizeof(size);
        return 1 + sizeof(size);
    }

    template<typename T, typename WebSocketFrame>
    size_t encodeParameter(T&& t, WebSocketFrame& frame) {
        using namespace std;
        using ParamType = remove_cvref_t<T>;
        setParametersCount(getParametersCount() + 1);
        size_t parameterDataSize = 0;

        if constexpr (is_printable<T>::value) {
            std::cout << "Param: " << typeName<T>() << " -> " << typeName<ParamType>() << " : " << t << "\n";
        }

        [[maybe_unused]] auto encodeString = [&, this ](const char* str, size_t size) constexpr {
            if (size < 256)
                parameterDataSize += encodeType(msg::CodedType::smallString, static_cast<uint8_t>(size), frame);
            else
                parameterDataSize += encodeType(msg::CodedType::string, static_cast<uint16_t>(size), frame);
            frame.addBuffer(span(reinterpret_cast<const std::byte*>(str), size));
            parameterDataSize += size;

            std::cout << "-> encoded to string of size " << size << " in a span.";
        };

        if constexpr (is_same_v<ParamType, bool>) {
            frame.addBuffer(span(&buffer[encodeBufferIndex], 1));
            buffer[encodeBufferIndex++] = static_cast<byte>(t ? msg::CodedType::booleanTrue : msg::CodedType::booleanFalse);
            parameterDataSize += 1;
            std::cout << "-> encoded to bool of size " << 1 << " in a span.";
        }
        else if constexpr (is_arithmetic_v<ParamType>) {
            double number = t;
            frame.addBuffer(span(&buffer[encodeBufferIndex], 1 + sizeof(number)));
            buffer[encodeBufferIndex++] = static_cast<byte>(msg::CodedType::number);
            copy_n(reinterpret_cast<const byte*>(&number), sizeof(number), &buffer[encodeBufferIndex]);
            encodeBufferIndex += sizeof(number);
            parameterDataSize += 1 + sizeof(number);
            std::cout << "-> encoded to number of size " << sizeof(number) << " in a span.";
        }

        else if constexpr (is_array_v<ParamType>) {
            using ElementType = remove_all_extents_t<ParamType>;
            //   cout << "Array of " << typeName<ElementType>() << "\n";
            //   cout << typeName<ParamType>() << " is bounded : " << is_bounded_array_v<ParamType> << "\n";
            if constexpr (is_same_v<ElementType, char>) {
                if constexpr (is_bounded_array_v<ParamType>)
                    encodeString(t, extent_v<ParamType> - 1);
                else
                    encodeString(t, char_traits<char>::length(t));
            }
            else
                static_assert(is_same_v<ElementType, char>, "Arrays are not supported by JSFunction");
        }
        else if constexpr (is_same_v<ParamType, const char*>)
            encodeString(t, char_traits<char>::length(t));

        else if constexpr (is_same_v<ParamType, string> or is_same_v<ParamType, string_view>)
            encodeString(t.data(), t.size());

        else if constexpr (is_pointer_v<ParamType>)
            static_assert(!is_pointer_v<ParamType>, "Pointers cannot be used by JSFunction");

        else if constexpr (is_base_of_v<std::exception, ParamType>) {
            std::cout << "Exception : " << t.what() << '\n';
            auto textSize = char_traits<char>::length(t.what());
            parameterDataSize += encodeType(msg::CodedType::exception, static_cast<uint16_t>(textSize), frame);
            frame.addBuffer(span(reinterpret_cast<const std::byte*>(t.what()), textSize));
            parameterDataSize += textSize;
        }

        std::cout << "bufferIndex = " << encodeBufferIndex << " ,parameterDataSize = " << parameterDataSize << "\n";

        return parameterDataSize;
    }

    template<typename T>
    static void decodeParameter(T& param, std::span<const std::byte>& data) {
        if (data.size() == 0) throw std::runtime_error("Not enough data for msg::FunctionCall::decodeParameter");
        auto codedType = static_cast<CodedType>(data[0]);
        switch (codedType) {
        case CodedType::smallString:
            if constexpr (std::is_same_v<T, std::string>) {
                if (data.size() < 1u) throw std::runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                auto size = static_cast<size_t>(data[1]);
                if (data.size() < 2u + size)
                    throw std::runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                param = std::string(reinterpret_cast<const char*>(&data[2]), size);
                data = data.subspan(2 + size);
            }
            break;
        case CodedType::string:
            if constexpr (std::is_same_v<T, std::string>) {
                if (data.size() < 1u) throw std::runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                uint16_t size;
                std::copy_n(&data[1], 2, reinterpret_cast<std::byte*>(&size));
                if (data.size() < 3u + size)
                    throw std::runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                param = std::string(reinterpret_cast<const char*>(&data[3]), size);
                data = data.subspan(3 + size);
            }
            break;
        case CodedType::number:
            if constexpr (std::is_arithmetic_v<T>) {
                double value;
                if (data.size() < 1u + sizeof(value))
                    throw std::runtime_error("Erroneous 'number' data feeded to msg::FunctionCall::decodeParameter");
                std::copy_n(&data[1], sizeof(value), reinterpret_cast<std::byte*>(&value));
                param = static_cast<T>(value);
                data = data.subspan(1 + sizeof(value));
            }
            break;

        default: param = {};
        }
    }
};

/// Encodes FunctionCall return values (or exceptions)
class FunctionReturn : public FunctionCall {
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
