/// @date 04/02/2022 13:15:27
/// @author Ambroise Leclerc
/// @brief A functor which invokes a corresponding javascript function
#pragma once
#include "WebLink.hpp"
#include "http/WebSocket.hpp"
#include "tooling/HexDump.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include <iostream>

namespace webfront {

enum class Type : uint8_t {
    undefined,
    booleanTrue,  // opcode if boolean is true
    booleanFalse, // opcode if boolean is false
    number,       // opcode + 8 bytes IEEE754 floating point number
    smallString,  // opcode + 1 byte size
    string,       // opcode + 2 bytes size
};

std::string_view toString(Type t) {
    switch (t) {
    case Type::booleanTrue: return "boolean (true)";
    case Type::booleanFalse: return "boolean (false)";
    case Type::number: return "number";
    case Type::smallString: return "smallString";
    case Type::string: return "string";
    default: return "undefined";
    }
}

template<typename WebFront>
class JsFunction {
public:
    JsFunction(std::string_view functionName, WebFront& wf, WebLinkId linkId)
        : name(functionName), webFront(wf), webLinkId(linkId), bufferIndex(0), paramsCount(0) {}

    void operator()(auto&&... ts) {
        bufferIndex = 0;
        paramsCount = 0;

        websocket::Frame<typename WebFront::Net> frame{std::span(reinterpret_cast<const std::byte*>(command.header().data()), command.header().size())};
        encodeParam(name, frame);
        ((encodeParam(std::forward<decltype(ts)>(ts), frame)), ...);
        command.setParametersCount(static_cast<uint8_t>(paramsCount));
        command.setParametersDataSize(static_cast<uint32_t>(frame.payloadSize() - command.header().size()));

        std::cout << utils::hexDump(std::span(buffer.data(), bufferIndex)) << "\n";

        size_t bufCount = 0;
        for (auto& buf : frame.buffers)
            std::cout << bufCount++ << ": " << utils::hexDump(std::span(reinterpret_cast<const std::byte*>(buf.data()), buf.size())) << "\n";
        webFront.getLink(webLinkId).sendFrame(std::move(frame));
    }

private:
    std::string name;
    WebFront& webFront;
    WebLinkId webLinkId;
    static constexpr size_t maxParamsCount = 32;
    static constexpr size_t maxParamsDataSize = 5; // 1 byte for type, 1-4 bytes for value
    std::array<std::byte, maxParamsCount * maxParamsDataSize> buffer;
    msg::CallJsFunction command;
    size_t bufferIndex, paramsCount;

    template<typename T>
    constexpr auto typeName() {
#if defined(_MSC_VER)
        std::string_view tName = __FUNCSIG__, prefix = "auto __cdecl JSFunction::typeName<", suffix = ">(void)";
#elif __clang__
        std::string_view tName = __PRETTY_FUNCTION__, prefix = "auto webfront::JSFunction::typeName() [T = ", suffix = "]";
#elif defined(__GNUC__)
        std::string_view tName = __PRETTY_FUNCTION__, prefix = "constexpr auto webfront::JSFunction::typeName() [with T = ", suffix = "]";
#endif
        tName.remove_prefix(prefix.size());
        tName.remove_suffix(suffix.size());
        return tName;
    }

    template<typename T>
    void encodeParam(T&& t, websocket::Frame<typename WebFront::Net>& frame) {
        using namespace std;
        using ParamType = std::remove_cvref_t<T>;
        paramsCount++;

        std::cout << "Param " << paramsCount << ": " << typeName<T>() << " -> " << typeName<ParamType>() << " : " << t << "\n";
        [[maybe_unused]] auto encodeString = [ this, &frame ](const char* str, size_t size) constexpr {
            if (size < 256) {
                frame.addBuffer(std::span(&buffer[bufferIndex], 2));
                buffer[bufferIndex++] = static_cast<std::byte>(Type::smallString);
                buffer[bufferIndex++] = static_cast<std::byte>(size);
            }
            else {
                frame.addBuffer(std::span(&buffer[bufferIndex], 3));
                buffer[bufferIndex++] = static_cast<std::byte>(Type::string);
                auto size16 = static_cast<uint16_t>(size);
                std::copy_n(reinterpret_cast<const std::byte*>(&size16), sizeof(size16), &buffer[bufferIndex]);
                bufferIndex += sizeof(size16);
            }
            frame.addBuffer(std::span(reinterpret_cast<const std::byte*>(str), size));
        };

        if constexpr (std::is_same_v<ParamType, bool>) {
            frame.addBuffer(std::span(&buffer[bufferIndex], 1));
            buffer[bufferIndex++] = static_cast<std::byte>(t ? Type::booleanTrue : Type::booleanFalse);
        }
        else if constexpr (std::is_arithmetic_v<ParamType>) {
            double number = t;
            frame.addBuffer(std::span(&buffer[bufferIndex], 1 + sizeof(number)));
            buffer[bufferIndex++] = static_cast<std::byte>(Type::number);
            std::copy_n(reinterpret_cast<const std::byte*>(&number), sizeof(number), &buffer[bufferIndex]);
            bufferIndex += sizeof(number);
        }

        else if constexpr (std::is_array_v<ParamType>) {
            using ElementType = std::remove_all_extents_t<ParamType>;
            cout << "Array of " << typeName<ElementType>() << "\n";
            cout << typeName<ParamType>() << " is bounded : " << is_bounded_array_v<ParamType> << "\n";
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
    }
};

} // namespace webfront
