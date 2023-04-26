/// @date 01/03/2022 22:39:42
/// @author Ambroise Leclerc
/// @brief Provides access to local or virtual file system.
#pragma once

#include "../details/C++23Support.hpp"
#include "tooling/Logger.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace webfront::filesystem {

class File {
public:
    File(std::span<const uint64_t> input, size_t fileSize, std::string contentEncoding = "")
        : data(input), readIndex(0), lastReadCount(0), size(fileSize), eofBit(false), badBit(false),
          encoding(std::move(contentEncoding)) {}
    File() = delete;

    File& read(std::span<char> s) { return read(s.data(), s.size()); }
    [[nodiscard]] bool isEncoded() const { return !encoding.empty(); }
    [[nodiscard]] std::string_view getEncoding() const { return encoding; }

    // fstream interface
    [[nodiscard]] bool bad() const { return badBit; }
    [[nodiscard]] bool eof() const { return eofBit; }
    [[nodiscard]] bool fail() const { return false; }
    [[nodiscard]] size_t gcount() const { return lastReadCount; }
    [[nodiscard]] size_t tellg() const { return readIndex; }
    [[nodiscard]] explicit operator bool() const { return !fail(); }
    [[nodiscard]] bool operator!() const { return eof() || bad(); }
    void seekg(size_t index) { readIndex = index; }
    void clear() {
        eofBit = false;
        badBit = false;
    }
    File& get(char* s, size_t count) { return read(s, count); }
    File& read(char* s, size_t count) {
        constexpr auto bytesPerInt = sizeof(decltype(data)::value_type);
        UIntByte chunk;
        for (size_t index = 0; index < count; ++index) {
            if (eof()) {
                badBit = true;
                lastReadCount = index;
                return *this;
            }

            if (readIndex % bytesPerInt == 0) chunk = convert(data[readIndex / bytesPerInt]);

            s[index] = static_cast<char>(chunk.byte[readIndex % bytesPerInt]);
            readIndex++;
            if (readIndex == size) eofBit = true;
        }
        lastReadCount = count;
        return *this;
    }

private:
    std::span<const uint64_t> data;
    size_t readIndex, lastReadCount, size;
    bool eofBit, badBit;
    const std::string encoding;

    union UIntByte {
        uint64_t uInt;
        uint8_t byte[sizeof(uInt)];
    };

    static UIntByte convert(uint64_t input) {
        UIntByte result;
        uint64_t bigEndian;
        if constexpr (std::endian::native == std::endian::little)
            bigEndian = std::byteswap(input);
        else
            bigEndian = input;
        std::memcpy(result.byte, &bigEndian, sizeof(bigEndian));
        return result;
    }
};

class NativeFS {};

} // namespace webfront::filesystem
