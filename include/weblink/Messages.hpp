/// @date 13/02/2022 21:24:42
/// @author Ambroise Leclerc
/// @brief Messages exchanged between webfront clients and server
#pragma once

#include "../http/BuffersPolicy.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace webfront::msg {

enum class JSEndian : uint8_t { little = 0, big = 1, mixed = little + big };
enum class TxtOpcode : uint8_t { debugLog, injectScript };

enum class Command : uint8_t {
    handshake,
    ack,
    textCommand,
    callFunction,    // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
    functionReturn,  // 1:Command  1:ParamsCount 2:padding  4:ParamsDataSize
};

enum class CodedType : uint8_t {
    undefined,
    booleanTrue,   // opcode if boolean is true
    booleanFalse,  // opcode if boolean is false
    number,        // opcode + 8 bytes IEEE754 floating point number
    smallString,   // opcode + 1 byte size
    string,        // opcode + 2 bytes size
    exception,     // opcode + 2 bytes size
    smallArrayU8,  // opcode + 1 byte size,
    arrayU8,       // opcode + 4 bytes size,
    array8,        // opcode + 4 bytes size,
    arrayU16,      // opcode + 4 bytes size,
    array16,       // opcode + 4 bytes size,
    arrayU32,      // opcode + 4 bytes size,
    array32,       // opcode + 4 bytes size,
    arrayU64,      // opcode + 4 bytes size,
    array64,       // opcode + 4 bytes size,
    arrayFloat,    // opcode + 4 bytes size,
    arrayDouble,   // opcode + 4 bytes size,
    tuple,         // opcode + 1 byte for number of parameters which constitute the tuple
};

inline std::string_view toString(CodedType t) {
    switch (t) {
        case CodedType::booleanTrue:
            return "boolean (true)";
        case CodedType::booleanFalse:
            return "boolean (false)";
        case CodedType::number:
            return "number";
        case CodedType::smallString:
            return "smallString";
        case CodedType::string:
            return "string";
        case CodedType::exception:
            return "exception";
        case CodedType::smallArrayU8:
            return "smallArrayU8";
        case CodedType::arrayU8:
            return "arrayU8";
        case CodedType::array8:
            return "array8";
        case CodedType::arrayU16:
            return "arrayU16";
        case CodedType::array16:
            return "array16";
        case CodedType::arrayU32:
            return "arrayU32";
        case CodedType::array32:
            return "array32";
        case CodedType::arrayU64:
            return "arrayU64";
        case CodedType::array64:
            return "array64";
        case CodedType::arrayFloat:
            return "arrayFloat";
        case CodedType::arrayDouble:
            return "arrayDouble";
        case CodedType::tuple:
            return "tuple";
        default:
            return "undefined";
    }
}

template <typename _Tp, typename dummy = void>
struct is_printable : std::false_type {};

template <typename T>
struct is_printable<T, std::void_t<decltype(std::cout << std::declval<T>())>> : std::true_type {};

template <typename T, typename U>
inline constexpr bool is_printable_v = is_printable<T, U>::value;

template <typename>
struct is_tuple : std::false_type {};

template <typename... T>
struct is_tuple<std::tuple<T...>> : std::true_type {};

template <typename T>
inline constexpr bool is_tuple_v = is_tuple<T>::value;

template <typename T>
inline constexpr bool is_numeric_array_element_v = std::is_same_v<std::remove_cv_t<T>, std::uint8_t> || std::is_same_v<std::remove_cv_t<T>, std::int8_t>
                                                   || std::is_same_v<std::remove_cv_t<T>, std::uint16_t> || std::is_same_v<std::remove_cv_t<T>, std::int16_t>
                                                   || std::is_same_v<std::remove_cv_t<T>, std::uint32_t> || std::is_same_v<std::remove_cv_t<T>, std::int32_t>
                                                   || std::is_same_v<std::remove_cv_t<T>, std::uint64_t> || std::is_same_v<std::remove_cv_t<T>, std::int64_t>
                                                   || std::is_same_v<std::remove_cv_t<T>, float> || std::is_same_v<std::remove_cv_t<T>, double>;

template <typename T>
struct numeric_array_traits {
    static constexpr bool supported = false;
};

template <typename T, typename Allocator>
struct numeric_array_traits<std::vector<T, Allocator>> {
    static constexpr bool supported = is_numeric_array_element_v<T>;
    using element_type              = T;
};

template <typename T, std::size_t Size>
struct numeric_array_traits<std::array<T, Size>> {
    static constexpr bool supported = is_numeric_array_element_v<T>;
    using element_type              = T;
};

template <typename T, std::size_t Extent>
struct numeric_array_traits<std::span<T, Extent>> {
    static constexpr bool supported = is_numeric_array_element_v<T>;
    using element_type              = T;
};

template <typename T>
inline constexpr bool is_numeric_array_v = numeric_array_traits<std::remove_cvref_t<T>>::supported;

template <typename T>
using numeric_array_element_t = typename numeric_array_traits<std::remove_cvref_t<T>>::element_type;

template <typename T>
struct is_vector : std::false_type {};

template <typename T, typename Allocator>
struct is_vector<std::vector<T, Allocator>> : std::true_type {};

template <typename T>
inline constexpr bool is_vector_v = is_vector<std::remove_cvref_t<T>>::value;

template <typename T>
struct is_std_array : std::false_type {};

template <typename T, std::size_t Size>
struct is_std_array<std::array<T, Size>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_array_v = is_std_array<std::remove_cvref_t<T>>::value;

template <typename T>
consteval CodedType numericArrayCode() {
    using Element = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<Element, std::uint8_t>)
        return CodedType::arrayU8;
    else if constexpr (std::is_same_v<Element, std::int8_t>)
        return CodedType::array8;
    else if constexpr (std::is_same_v<Element, std::uint16_t>)
        return CodedType::arrayU16;
    else if constexpr (std::is_same_v<Element, std::int16_t>)
        return CodedType::array16;
    else if constexpr (std::is_same_v<Element, std::uint32_t>)
        return CodedType::arrayU32;
    else if constexpr (std::is_same_v<Element, std::int32_t>)
        return CodedType::array32;
    else if constexpr (std::is_same_v<Element, std::uint64_t>)
        return CodedType::arrayU64;
    else if constexpr (std::is_same_v<Element, std::int64_t>)
        return CodedType::array64;
    else if constexpr (std::is_same_v<Element, float>)
        return CodedType::arrayFloat;
    else if constexpr (std::is_same_v<Element, double>)
        return CodedType::arrayDouble;
    else
        static_assert(!sizeof(T), "Unsupported WebFront numeric array element type");
}

template <typename T>
class MessageBase {
public:
    [[nodiscard]] std::span<const std::byte> header() const {
        return {reinterpret_cast<const std::byte*>(this), sizeof(typename T::Header)};
    }
    [[nodiscard]] std::span<const std::byte> payload() const {
        return {header().data() + sizeof(typename T::Header), static_cast<const T*>(this)->getPayloadSize()};
    }
    [[nodiscard]] size_t getPayloadSize() const {
        return 0;
    };

    static const T* castFromRawData(std::span<const std::byte> data) {
        if (data.size() < sizeof(typename T::Header))
            throw std::runtime_error("Not enough data to form a message Header");
        auto message = reinterpret_cast<const T*>(data.data());
        if ((data.size() + message->getPayloadSize()) < sizeof(typename T::Header))
            throw std::runtime_error("Not enough data to form a complete message");
        return message;
    }
};

// First message sent by JS client to WebFront
class Handshake : public MessageBase<Handshake> {
    struct Header {
        Command  command = Command::handshake;
        JSEndian endian  = {};
    } head;
    static_assert(sizeof(Header) == 2, "Handshake header has to be 2 bytes long");
    friend class MessageBase<Handshake>;

public:
    [[nodiscard]] JSEndian getEndian() const {
        return head.endian;
    }
};

// Message sent by WebFront to acknoledge a Handshake message
class Ack : public MessageBase<Ack> {
    struct Header {
        Command  command = Command::ack;
        JSEndian endian  = std::endian::native == std::endian::little ? JSEndian::little : JSEndian::big;
    } head;
    static_assert(sizeof(Header) == 2, "Ack header has to be 2 bytes long");
    friend class MessageBase<Ack>;
};

/// Commands with a text payload
class TextCommand : public MessageBase<TextCommand> {
    struct Header {
        Command   command = Command::textCommand;
        TxtOpcode opcode;
        uint8_t   lengthHi, lengthLo;  // Payload length = (256 * lengthHi) + lengthLo
    } head;
    static_assert(sizeof(Header) == 4, "TextCommand header has to be 4 bytes long");

    std::span<const std::byte> payloadSpan;
    friend class MessageBase<TextCommand>;

public:
    TextCommand(TxtOpcode opcode, std::string_view text)
        : head{.opcode = opcode, .lengthHi = static_cast<uint8_t>(text.size() >> 8), .lengthLo = static_cast<uint8_t>(text.size())},
          payloadSpan{reinterpret_cast<const std::byte*>(text.data()), text.size()} {}

    [[nodiscard]] std::span<const std::byte> payload() const {
        return payloadSpan;
    }
    [[nodiscard]] size_t getPayloadSize() const {
        return static_cast<uint16_t>(256 * head.lengthHi + head.lengthLo);
    }
};

/// Shared header layout for function call/return messages
template <Command Cmd>
struct FunctionCallHeader {
    Command                command         = Cmd;
    uint8_t                parametersCount = 0;  // parameter 0 is the function name so parametersCount is always at least 1
    std::array<uint8_t, 2> padding{};
    uint32_t               parametersDataSize = 0;
};
static_assert(sizeof(FunctionCallHeader<Command::callFunction>) == 8, "FunctionCall header has to be 8 bytes long");

/// Shared encode/decode logic for FunctionCall and FunctionReturn
template <typename Derived, Command Cmd, http::BuffersPolicyType Policy>
class FunctionCallBase : public MessageBase<Derived> {
public:
    using Header = FunctionCallHeader<Cmd>;

protected:
    Header                                                head{};
    std::array<std::byte, Policy::functionCallBufferSize> buffer{};
    size_t                                                encodeBufferIndex = 0;
    friend class MessageBase<Derived>;

    template <typename SizeType, typename WebSocketFrame>
    void encodeTypeHeader(msg::CodedType type, SizeType size, WebSocketFrame& frame) {
        if (encodeBufferIndex + 1 + sizeof(size) > buffer.size())
            throw http::BufferOverflowException("FunctionCall encode buffer overflow");
        frame.addBuffer(std::span(&buffer[encodeBufferIndex], 1 + sizeof(size)));
        buffer[encodeBufferIndex++] = static_cast<std::byte>(type);
        std::copy_n(reinterpret_cast<const std::byte*>(&size), sizeof(size), &buffer[encodeBufferIndex]);
        encodeBufferIndex += sizeof(size);
        incrementPayloadSize(1 + sizeof(size));
    }

    template <typename WebSocketFrame>
    void encodeString(const char* str, size_t size, WebSocketFrame& frame) {
        using namespace std;
        if (size < 256)
            encodeTypeHeader(msg::CodedType::smallString, static_cast<uint8_t>(size), frame);
        else
            encodeTypeHeader(msg::CodedType::string, static_cast<uint16_t>(size), frame);
        frame.addBuffer(span(reinterpret_cast<const std::byte*>(str), size));
        incrementPayloadSize(size);
        std::cout << "-> encoded to string of size " << size << " in a span.\n";
    }

    template <typename WebSocketFrame>
    void encodeBool(bool value, WebSocketFrame& frame) {
        using namespace std;
        if (encodeBufferIndex + 1 > buffer.size())
            throw http::BufferOverflowException("FunctionCall encode buffer overflow");
        frame.addBuffer(span(&buffer[encodeBufferIndex], 1));
        buffer[encodeBufferIndex++] = static_cast<byte>(value ? msg::CodedType::booleanTrue : msg::CodedType::booleanFalse);
        incrementPayloadSize(1);
        std::cout << "-> encoded to bool of size 1 in a span.";
    }

    template <typename WebSocketFrame>
    void encodeNumber(double number, WebSocketFrame& frame) {
        using namespace std;
        if (encodeBufferIndex + 1 + sizeof(number) > buffer.size())
            throw http::BufferOverflowException("FunctionCall encode buffer overflow");
        frame.addBuffer(span(&buffer[encodeBufferIndex], 1 + sizeof(number)));
        buffer[encodeBufferIndex++] = static_cast<byte>(msg::CodedType::number);
        copy_n(reinterpret_cast<const byte*>(&number), sizeof(number), &buffer[encodeBufferIndex]);
        encodeBufferIndex += sizeof(number);
        incrementPayloadSize(1 + sizeof(number));
        std::cout << "-> encoded to number of size " << sizeof(number) << " in a span.";
    }

    template <typename WebSocketFrame>
    void encodeException(const std::exception& e, WebSocketFrame& frame) {
        using namespace std;
        std::cout << "Exception : " << e.what() << '\n';
        auto textSize = char_traits<char>::length(e.what());
        encodeTypeHeader(msg::CodedType::exception, static_cast<uint16_t>(textSize), frame);
        frame.addBuffer(span(reinterpret_cast<const std::byte*>(e.what()), textSize));
        incrementPayloadSize(textSize);
    }

    template <typename Array, typename WebSocketFrame>
    void encodeNumericArray(const Array& value, WebSocketFrame& frame) {
        using Element           = std::remove_cv_t<numeric_array_element_t<Array>>;
        const auto elementCount = value.size();
        if (elementCount > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("WebFront arrays cannot exceed 4294967295 elements");
        if (elementCount > std::numeric_limits<std::size_t>::max() / sizeof(Element))
            throw std::length_error("WebFront array byte size overflow");

        if constexpr (std::is_same_v<Element, std::uint8_t>) {
            if (elementCount < 256)
                encodeTypeHeader(CodedType::smallArrayU8, static_cast<std::uint8_t>(elementCount), frame);
            else
                encodeTypeHeader(CodedType::arrayU8, static_cast<std::uint32_t>(elementCount), frame);
        } else
            encodeTypeHeader(numericArrayCode<Element>(), static_cast<std::uint32_t>(elementCount), frame);

        const auto byteCount = elementCount * sizeof(Element);
        frame.addBuffer(std::span(reinterpret_cast<const std::byte*>(value.data()), byteCount));
        incrementPayloadSize(byteCount);
    }

public:
    void reset() {
        head.parametersCount    = 0;
        head.parametersDataSize = 0;
        encodeBufferIndex       = 0;
    }
    void setParametersCount(uint8_t parametersCount) {
        head.parametersCount = parametersCount;
    }
    void setPayloadSize(uint32_t size) {
        head.parametersDataSize = size;
    }
    [[nodiscard]] uint8_t getParametersCount() const {
        return head.parametersCount;
    }
    [[nodiscard]] size_t getPayloadSize() const {
        return head.parametersDataSize;
    }
    void incrementPayloadSize(auto value) {
        setPayloadSize(static_cast<uint32_t>(getPayloadSize()) + static_cast<uint32_t>(value));
    }
    [[nodiscard]] std::tuple<std::string, std::span<const std::byte>> getFunctionName() const {
        std::string functionName;
        auto        data = this->payload();
        decodeParameter(functionName, data);
        return {functionName, data};
    }

    template <typename T, typename WebSocketFrame>
    void encodeParameter(T&& t, WebSocketFrame& frame) {
        using namespace std;
        using ParamType = remove_cvref_t<T>;
        setParametersCount(getParametersCount() + 1);

        if constexpr (is_tuple_v<ParamType>) {
            uint8_t nbParams = static_cast<uint8_t>(tuple_size<ParamType>::value);
            encodeTypeHeader(msg::CodedType::tuple, nbParams, frame);
            std::apply(
                [&](auto&... tupleArgs) {
                    ((encodeParameter(tupleArgs, frame)), ...);
                },
                t);
        } else if constexpr (is_same_v<ParamType, bool>)
            encodeBool(t, frame);
        else if constexpr (is_arithmetic_v<ParamType>)
            encodeNumber(static_cast<double>(t), frame);
        else if constexpr (is_numeric_array_v<ParamType>)
            encodeNumericArray(t, frame);
        else if constexpr (is_array_v<ParamType>) {
            using ElementType = remove_all_extents_t<ParamType>;
            static_assert(is_same_v<ElementType, char>, "Arrays are not supported by JSFunction");
            if constexpr (is_bounded_array_v<ParamType>)
                encodeString(t, extent_v<ParamType> - 1, frame);
            else
                encodeString(t, char_traits<char>::length(t), frame);
        } else if constexpr (is_same_v<ParamType, const char*>)
            encodeString(t, char_traits<char>::length(t), frame);
        else if constexpr (is_same_v<ParamType, string> or is_same_v<ParamType, string_view>)
            encodeString(t.data(), t.size(), frame);
        else if constexpr (is_pointer_v<ParamType>)
            static_assert(!is_pointer_v<ParamType>, "Pointers cannot be used by JSFunction");
        else if constexpr (is_base_of_v<std::exception, ParamType>)
            encodeException(t, frame);

        std::cout << "bufferIndex = " << encodeBufferIndex << " ,payloadSize = " << getPayloadSize() << "\n";
    }

    template <typename T>
    static void decodeParameter(T& param, std::span<const std::byte>& data) {
        using namespace std;
        cout << "decodeParameter with data size " << data.size() << " bytes\n";
        if (data.size() == 0)
            throw runtime_error("Not enough data for msg::FunctionCall::decodeParameter");
        auto codedType = static_cast<CodedType>(data[0]);
        switch (codedType) {
            case CodedType::booleanTrue:
            case CodedType::booleanFalse:
                if constexpr (is_same_v<T, bool>) {
                    param = codedType == CodedType::booleanTrue;
                    data  = data.subspan(1);
                } else
                    throw runtime_error("Wrong parameter type: bool expected");
                break;
            case CodedType::smallString:
                if constexpr (is_same_v<T, string>) {
                    if (data.size() < 1u)
                        throw runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                    auto size = static_cast<size_t>(data[1]);
                    if (data.size() < 2u + size)
                        throw runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                    param = string(reinterpret_cast<const char*>(&data[2]), size);
                    data  = data.subspan(2 + size);
                }
                break;
            case CodedType::string:
            case CodedType::exception:
                if constexpr (is_same_v<T, string>) {
                    if (data.size() < 1u)
                        throw runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                    uint16_t size;
                    copy_n(&data[1], 2, reinterpret_cast<byte*>(&size));
                    if (data.size() < 3u + size)
                        throw runtime_error("Erroneous data feeded to msg::FunctionCall::decodeParameter");
                    param = string(reinterpret_cast<const char*>(&data[3]), size);
                    data  = data.subspan(3 + size);
                }
                break;
            case CodedType::number:
                if constexpr (is_arithmetic_v<T>) {
                    double value;
                    if (data.size() < 1u + sizeof(value))
                        throw runtime_error("Erroneous 'number' data feeded to msg::FunctionCall::decodeParameter");
                    copy_n(&data[1], sizeof(value), reinterpret_cast<byte*>(&value));
                    param = static_cast<T>(value);
                    data  = data.subspan(1 + sizeof(value));
                }
                break;
            case CodedType::smallArrayU8:
            case CodedType::arrayU8:
            case CodedType::array8:
            case CodedType::arrayU16:
            case CodedType::array16:
            case CodedType::arrayU32:
            case CodedType::array32:
            case CodedType::arrayU64:
            case CodedType::array64:
            case CodedType::arrayFloat:
            case CodedType::arrayDouble:
                if constexpr (is_numeric_array_v<T> && (is_vector_v<T> || is_std_array_v<T>))
                    decodeNumericArray(param, data);
                else
                    throw runtime_error("Wrong parameter type: owning numeric array expected");
                break;
            case CodedType::tuple:
                if constexpr (is_tuple_v<T>) {
                    auto tupleSize = static_cast<size_t>(data[1]);
                    if (tuple_size_v<T> != tupleSize)
                        throw runtime_error("Parameter is a "s + to_string(tuple_size_v<T>) + " elements tuple but decoded tuple only has "
                                            + to_string(tupleSize) + " elements.");
                    cout << "tuple expected\n";
                    data = data.subspan(2);
                    std::apply(
                        [&](auto&... tupleArgs) {
                            ((decodeParameter(tupleArgs, data)), ...);
                        },
                        param);
                } else
                    throw runtime_error("Wrong parameter type : std::tuple expected");
                break;

            default:
                throw runtime_error("Unsupported coded parameter type");
        }
    }

private:
    template <typename Array>
    static void decodeNumericArray(Array& param, std::span<const std::byte>& data) {
        using Element         = std::remove_cv_t<numeric_array_element_t<Array>>;
        const auto type       = static_cast<CodedType>(data.front());
        const bool smallUint8 = type == CodedType::smallArrayU8;
        if (smallUint8 && !std::is_same_v<Element, std::uint8_t>)
            throw std::runtime_error("Typed array element type does not match its wire type");
        if (!smallUint8 && type != numericArrayCode<Element>())
            throw std::runtime_error("Typed array element type does not match its wire type");

        const std::size_t headerSize = smallUint8 ? 2 : 5;
        if (data.size() < headerSize)
            throw std::runtime_error("Truncated typed array header");

        std::uint32_t elementCount{};
        if (smallUint8)
            elementCount = static_cast<std::uint8_t>(data[1]);
        else
            std::memcpy(&elementCount, data.data() + 1, sizeof(elementCount));

        if (elementCount > std::numeric_limits<std::size_t>::max() / sizeof(Element))
            throw std::runtime_error("Typed array byte size overflow");
        const auto byteCount = static_cast<std::size_t>(elementCount) * sizeof(Element);
        if (data.size() - headerSize < byteCount)
            throw std::runtime_error("Truncated typed array payload");

        if constexpr (is_vector_v<Array>)
            param.resize(elementCount);
        else if (param.size() != elementCount)
            throw std::runtime_error("Fixed-size array length does not match typed array payload");

        if (byteCount != 0)
            std::memcpy(param.data(), data.data() + headerSize, byteCount);
        data = data.subspan(headerSize + byteCount);
    }
};

/// Encodes a list of parameters : parameter 0 is the function name. Sent either by WebFront or by the JS Client
template <http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
class FunctionCall : public FunctionCallBase<FunctionCall<Policy>, Command::callFunction, Policy> {
    friend class MessageBase<FunctionCall<Policy>>;
    friend class FunctionCallBase<FunctionCall<Policy>, Command::callFunction, Policy>;
};

/// Encodes FunctionCall return values (or exceptions)
template <http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
class FunctionReturn : public FunctionCallBase<FunctionReturn<Policy>, Command::functionReturn, Policy> {
    friend class MessageBase<FunctionReturn<Policy>>;
    friend class FunctionCallBase<FunctionReturn<Policy>, Command::functionReturn, Policy>;
};

}  // namespace webfront::msg
