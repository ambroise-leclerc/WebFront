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
#include <string>
#include <string_view>
#include <system_error>

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
    File(std::ifstream ifstream) : fileType{FileType::nativeFStream}, fstream{std::move(ifstream)} { measureStream(); }
    /// A native file, with the validator its path provides (see entityTag()).
    File(std::ifstream ifstream, const std::filesystem::path& source) : File(std::move(ifstream)) {
        std::error_code error;
        const auto      modified = std::filesystem::last_write_time(source, error);
        if (!error && streamSize) entityTagValue = "\"" + std::to_string(*streamSize) + "-" + std::to_string(static_cast<long long>(modified.time_since_epoch().count())) + "\"";
    }

    [[nodiscard]] bool isEncoded() const { return !encoding.empty(); }
    [[nodiscard]] std::string_view getEncoding() const { return encoding; }
    [[nodiscard]] bool eof() const { return eofBit; }

    /// The number of bytes this file yields, when known before reading it (always for embedded data, and for a
    /// native file whose stream can seek). A server uses it to announce a length and stream the file in chunks.
    [[nodiscard]] std::optional<std::uint64_t> knownSize() const { return fileType == FileType::staticData ? std::optional<std::uint64_t>{size} : streamSize; }

    /// A strong entity tag (RFC 9110 §8.8.3), when the file can tell its version: a native file's size and
    /// modification time. Embedded data and providers that do not know their version have none, and a range
    /// request conditioned by `If-Range` is then answered in full.
    [[nodiscard]] const std::optional<std::string>& entityTag() const { return entityTagValue; }

    /// Positions the next read at `offset` bytes from the start of the content (the position the file had when it
    /// was built), when knownSize() is set. An offset past the end positions at the end.
    /// @return false when the file cannot seek, or the stream failed; the position is then unspecified.
    [[nodiscard]] bool seek(std::uint64_t offset) {
        if (fileType == FileType::staticData) {
            readIndex = static_cast<size_t>(std::min<std::uint64_t>(offset, size));
            eofBit    = readIndex == size;
            return true;
        }
        if (!streamSize || !fstream.is_open()) return false;
        const auto target = std::min(offset, *streamSize);
        fstream.clear();
        fstream.seekg(streamOrigin + static_cast<std::streamoff>(target));
        eofBit = false;
        return static_cast<bool>(fstream);
    }

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
    std::optional<std::uint64_t> streamSize;
    std::streamoff streamOrigin{};
    std::optional<std::string> entityTagValue;
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

    // The bytes left from the stream's current position, which the caller may have moved past a prefix.
    void measureStream() {
        if (!fstream.is_open()) return;
        const auto start = fstream.tellg();
        fstream.seekg(0, std::ios::end);
        const auto end = fstream.tellg();
        fstream.seekg(start);
        if (start >= 0 && end >= start && fstream) {
            streamSize   = static_cast<std::uint64_t>(end - start);
            streamOrigin = static_cast<std::streamoff>(start);
        }
        else fstream.clear();
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
