/// @date 01/03/2023 22:39:42
/// @author Ambroise Leclerc
/// @brief Provides access to local or virtual file system.
#pragma once

#include "../details/C++23Support.hpp"
#include <webfront/tooling/Logger.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <ios>
#include <optional>
#include <span>
#include <string_view>

namespace webfront::fs {

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

class File {
    enum class FileType { nativeFStream, staticData };
public:
    File(EncodedData auto t) : File(decltype(t)::data, decltype(t)::dataSize, decltype(t)::encoding) {}
    File(RawData auto t) : File(decltype(t)::data, decltype(t)::dataSize) {}
    File(std::span<const uint64_t> input, size_t fileSize, std::string_view contentEncoding = "")
        : fileType{FileType::staticData}, data(input), size(fileSize), eofBit(fileSize==0), encoding(contentEncoding) {}
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

    // The generated headers pack the bytes into big-endian 64-bit words. Each byte is taken from
    // its word by position, so a read may stop in the middle of a word.
    size_t readData(std::span<char> buffer) {
        constexpr size_t bitsPerByte = 8;
        constexpr size_t bytesPerInt = sizeof(decltype(data)::value_type);
        const auto       count       = std::min(buffer.size(), size - readIndex);
        for (size_t index = 0; index < count; ++index, ++readIndex) {
            const auto shift = (bytesPerInt - 1 - readIndex % bytesPerInt) * bitsPerByte;
            buffer[index]    = static_cast<char>(static_cast<uint8_t>(data[readIndex / bytesPerInt] >> shift));
        }
        eofBit = readIndex == size;
        return count;
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
