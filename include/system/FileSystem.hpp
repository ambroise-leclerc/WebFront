/// @date 01/03/2022 22:39:42
/// @author Ambroise Leclerc
/// @brief Provides access to local or virtual file system.
#pragma once

#include "../details/C++23Support.hpp"
#include "tooling/Logger.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string_view>

namespace webfront::fs {

namespace {
template<typename T>
concept HasEncoding = requires(T t) { T::encoding; };

template<typename T>
concept IsData = requires(T t) {
    T::data;
    T::dataSize;
};

template<typename T>
concept EncodedData = IsData<T> && HasEncoding<T>;

template<typename T>
concept RawData = IsData<T>;
} // namespace

class File {
    enum class FileType { nativeFStream, staticData };
public:
    File(EncodedData auto t) : File(decltype(t)::data, decltype(t)::dataSize, decltype(t)::encoding) {}
    File(RawData auto t) : File(decltype(t)::data, decltype(t)::dataSize) {}
    File(std::span<const uint64_t> input, size_t fileSize, std::string_view contentEncoding = "")
        : fileType{FileType::staticData}, data(input), size(fileSize), encoding(contentEncoding) {}
    File(std::ifstream ifstream) : fileType{FileType::nativeFStream}, fstream{std::move(ifstream)} {}

    [[nodiscard]] bool isEncoded() const { return !encoding.empty(); }
    [[nodiscard]] std::string_view getEncoding() const { return encoding; }
    [[nodiscard]] bool eof() const { return eofBit; }

    // Extracts characters from file into given buffer until buffer size or end of file is reached.
    // @param buffer buffer which will receive extracted data
    // @return bytes read
    size_t read(std::span<char> buffer) {
        if (fileType == FileType::staticData) return readData(buffer);
        else return readFStream(buffer);
    }

private:
    FileType fileType;
    std::span<const uint64_t> data;
    size_t readIndex{}, size{};
    bool eofBit{false};
    const std::string encoding{};
    std::ifstream fstream;

    union UIntByte {
        uint64_t uInt;
        uint8_t byte[sizeof(uInt)];
    };

    size_t readData(std::span<char> buffer) {
        constexpr auto bytesPerInt = sizeof(decltype(data)::value_type);
        UIntByte chunk;
        for (size_t index = 0; index < buffer.size(); ++index) {
            if (eof()) return index;

            if (readIndex % bytesPerInt == 0) chunk = convert(data[readIndex / bytesPerInt]);

            buffer[index] = static_cast<char>(chunk.byte[readIndex % bytesPerInt]);
            readIndex++;
            if (readIndex == size) eofBit = true;
        }
        return buffer.size();
    }

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

    size_t readFStream(std::span<char> buffer) {
        fstream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (fstream.eof()) {
            eofBit = true;
            fstream.close();
        }
        return static_cast<size_t>(fstream.gcount());
    }
};

template<typename T>
concept Provider = requires(T t, std::filesystem::path filename) {
    { t.open(filename) } -> std::same_as<std::optional<File>>;
    requires std::constructible_from<T, std::filesystem::path>;
};

template<Provider ... FSs>
class Multi : FSs... {
public:
    Multi(std::filesystem::path docRoot) : FSs(docRoot)... {}
    std::optional<File> open(std::filesystem::path filename) {
        return openFile<FSs...>(filename);
    }

private:
    template<typename First, typename ... Rest>
    std::optional<File> openFile(std::filesystem::path filename) {
        auto file = this->First::open(filename);
        if constexpr (sizeof...(Rest) == 0) return file;
        else {
            if (file.has_value()) return file;
            return openFile<Rest...>(filename);
        }
    }
};

} // namespace webfront::fs
