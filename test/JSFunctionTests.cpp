#include <catch2/catch.hpp>


#include <tooling/HexDump.hpp>

#include <algorithm>
#include <array>

#include <iostream>

using namespace webfront;


enum class Type : uint8_t {
    undefined,
    booleanTrue,            // opcode if boolean is true
    booleanFalse,           // opcode if boolean is false
    number,                 // opcode + 8 bytes IEEE754 floating point number
    string,                 // opcode + 2 bytes size
    smallArray,             // opcode + 1 byte size
    bigarray,               // opcode + 4 bytes size
};

std::string_view toString(Type t) {
    switch (t) {
    case Type::booleanTrue: return "boolean (true)";
    case Type::booleanFalse: return "boolean (false)";
    case Type::number: return "number";
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

         ((encodeParam(ts)), ...); 
 
        std::cout << utils::hexDump(buffer) << "\n";

        for (size_t i = 0; i < paramIndex; ++i)
            std::cout << "Param " << i << ": " << utils::hexDump(paramSpans[i]) << "\n";
    }

private:
    static constexpr size_t maxParamsCount = 32;
    static constexpr size_t maxParamsDataSize = 5; // 1 byte for type, 1-4 bytes for value
    std::array<std::byte, maxParamsCount * maxParamsDataSize> buffer;
    size_t bufferIndex, paramIndex;    
    std::array<std::span<const std::byte>, maxParamsCount> paramSpans;

    template<typename T>
    void encodeParam(T t) {
        [[maybe_unused]] auto string = [this](const char* str, size_t size) {
            paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 3);
            buffer[bufferIndex++] = static_cast<std::byte>(Type::string);
            auto size16 = static_cast<uint16_t>(size);
            std::copy_n(reinterpret_cast<const std::byte*>(&size16), sizeof(size16), &buffer[bufferIndex]);
            bufferIndex += sizeof(size16);
            paramSpans[paramIndex++] = std::span(reinterpret_cast<const std::byte*>(str), size16);
        
        };


        if constexpr (std::is_same_v<T, bool>) {
            paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 1);
            buffer[bufferIndex++] = static_cast<std::byte>(t ? Type::booleanTrue : Type::booleanFalse);
        }
        else if constexpr (std::is_arithmetic_v<T>) {
            paramSpans[paramIndex++] = std::span(&buffer[bufferIndex], 8);
            buffer[bufferIndex++] = static_cast<std::byte>(Type::number);
            double number = t;
            std::copy_n(reinterpret_cast<const std::byte*>(&number), sizeof(number), &buffer[bufferIndex]);
            bufferIndex += sizeof(number);
        }
        else if constexpr (std::is_same_v<T, const char*>) string(t, std::char_traits<char>::length(t));
        else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) string(t.data(), t.size());
    }
};

using namespace std;


SCENARIO("JsFunction") {
    JsFunction print;
    static std::string text{"Bob l'éponge"};
    std::cout << utils::hexDump(text) << "\n";
    print(true, "Texte", 45, text);
    std::cout << utils::hexDump(text) << "\n";

}