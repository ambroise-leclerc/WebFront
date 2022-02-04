/// @date 04/02/2022 13:15:27
/// @author Ambroise Leclerc
/// @brief A functor which invokes a corresponding javascript function
#pragma once
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
    smallArray,   // opcode + 1 byte size
    bigarray,     // opcode + 4 bytes size
};

std::string_view toString(Type t) {
    switch (t) {
    case Type::booleanTrue: return "boolean (true)";
    case Type::booleanFalse: return "boolean (false)";
    case Type::number: return "number";
    case Type::smallString: return "smallString";
    case Type::string: return "string";
    case Type::smallArray: return "smallArray";
    case Type::bigarray: return "bigArray";
    default: return "undefined";
    }
}

class JsFunction {
public:
    void operator()(auto&&... ts) {
        bufferIndex = 0;
        paramIndex = 0;

        ((encodeParam(std::forward<decltype(ts)>(ts))), ...);

        std::cout << utils::hexDump(buffer) << "\n";

        for (size_t i = 0; i < paramIndex; ++i) std::cout << i << ": " << utils::hexDump(paramSpans[i]) << "\n";
    }

private:
    static constexpr size_t maxParamsCount = 32;
    static constexpr size_t maxParamsDataSize = 5; // 1 byte for type, 1-4 bytes for value
    std::array<std::byte, maxParamsCount * maxParamsDataSize> buffer;
    size_t bufferIndex, paramIndex;
    std::array<std::span<const std::byte>, maxParamsCount> paramSpans;

    template<typename T>
    constexpr auto typeName() {
#if defined(_MSC_VER)
        std::string_view name = __FUNCSIG__, prefix = "auto __cdecl JSFunction::typeName<", suffix = ">(void)";
#elif __clang__
        std::string_view name = __PRETTY_FUNCTION__, prefix = "auto JSFunction::typeName() [T = ", suffix = "]";
#elif defined(__GNUC__)
        std::string_view name = __PRETTY_FUNCTION__, prefix = "constexpr auto JSFunction::typeName() [with T = ", suffix = "]";
#endif
        name.remove_prefix(prefix.size());
        name.remove_suffix(suffix.size());
        return name;
    }

    template<typename T>
    void encodeParam(T&& t) {
        using namespace std;
        using ParamType = std::remove_cvref_t<T>;
        std::cout << typeName<T>() << " -> " << typeName<ParamType>() << " : " << t << "\n";
        [[maybe_unused]] auto encodeString = [this](const char* str, size_t size) constexpr {
            if (size < 256) {
                paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 2);
                buffer[bufferIndex++] = static_cast<std::byte>(Type::smallString);
                buffer[bufferIndex++] = static_cast<std::byte>(size);
            }
            else {
                paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 3);
                buffer[bufferIndex++] = static_cast<std::byte>(Type::string);
                auto size16 = static_cast<uint16_t>(size);
                std::copy_n(reinterpret_cast<const std::byte*>(&size16), sizeof(size16), &buffer[bufferIndex]);
                bufferIndex += sizeof(size16);
            }
            paramSpans[paramIndex++] = std::span(reinterpret_cast<const std::byte*>(str), size);
        };

        if constexpr (std::is_same_v<ParamType, bool>) {
            paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 1);
            buffer[bufferIndex++] = static_cast<std::byte>(t ? Type::booleanTrue : Type::booleanFalse);
        }
        else if constexpr (std::is_arithmetic_v<ParamType>) {
            paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 8);
            buffer[bufferIndex++] = static_cast<std::byte>(Type::number);
            double number = t;
            std::copy_n(reinterpret_cast<const std::byte*>(&number), sizeof(number), &buffer[bufferIndex]);
            bufferIndex += sizeof(number);
        }

        else if constexpr (std::is_array_v<ParamType>) {
            using ElementType = std::remove_all_extents_t<ParamType>;
            cout << "Array of " << typeName<ElementType>() << "\n";
            cout << typeName<ParamType>() << " is bounded : " << is_bounded_array_v<ParamType> << "\n";
            if constexpr (is_same_v<ElementType, char>)
                if constexpr (is_bounded_array_v<ParamType>)
                    encodeString(t, extent_v<ParamType> - 1);
                else
                    encodeString(t, char_traits<char>::length(t));
            else
                static_assert(false, "Arrays are not supported by JSFunction");
        }
        else if constexpr (is_same_v<ParamType, const char*>)
            encodeString(t, char_traits<char>::length(t));

        else if constexpr (is_same_v<ParamType, string> or is_same_v<ParamType, string_view>)
            encodeString(t.data(), t.size());

        else if constexpr (is_pointer_v<ParamType>)
            static_assert(false, "Pointers cannot be used by JSFunction");
    }
};

} // namespace webfront
