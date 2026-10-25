/** @brief Bounded, header-only gzip decoding for embedded HTTP assets (RFC 1951/1952). */
#pragma once

#include "BuffersPolicy.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace webfront::http::gzip {

struct InvalidGzip : std::exception {};

class GzipReader {
public:
    explicit GzipReader(std::string_view bytes) : input(bytes) {}

    unsigned bits(unsigned count) {
        unsigned value = 0;
        for (unsigned index = 0; index < count; ++index) {
            if (position == input.size())
                throw InvalidGzip{};
            value |= ((static_cast<unsigned char>(input[position]) >> bit) & 1u) << index;
            if (++bit == 8) {
                bit = 0;
                ++position;
            }
        }
        return value;
    }

    std::string_view readBytes(std::size_t count) {
        if (bit != 0)
            throw InvalidGzip{};
        if (count > input.size() - position)
            throw InvalidGzip{};
        const auto bytes = input.substr(position, count);
        position        += count;
        return bytes;
    }

    void align() {
        if (bit != 0) {
            bit = 0;
            ++position;
        }
    }

    std::size_t offset() const {
        return position;
    }
    bool done() const {
        return position == input.size();
    }

private:
    std::string_view input;
    std::size_t      position = 0;
    unsigned         bit      = 0;
};

class DeflateTree {
public:
    explicit DeflateTree(std::span<const unsigned> lengths, bool requireComplete = false) {
        countLengths(lengths);
        validate(lengths.size(), requireComplete);
        buildTable(lengths);
    }

    unsigned decode(GzipReader& input) const {
        unsigned code = 0, first = 0, index = 0;
        for (unsigned length = 1; length <= 15; ++length) {
            code |= input.bits(1);
            if (code >= first && code - first < counts[length])
                return sorted[index + code - first];
            index += counts[length];
            first  = (first + counts[length]) << 1;
            code <<= 1;
        }
        throw InvalidGzip{};
    }

private:
    void countLengths(std::span<const unsigned> lengths) {
        if (lengths.size() > sorted.size())
            throw InvalidGzip{};
        for (auto length : lengths) {
            if (length > 15)
                throw InvalidGzip{};
            ++counts[length];
        }
    }

    int unusedCodes() const {
        int remaining = 1;
        for (unsigned length = 1; length <= 15; ++length) {
            remaining = (remaining << 1) - static_cast<int>(counts[length]);
            if (remaining < 0)
                throw InvalidGzip{};
        }
        return remaining;
    }

    void validate(std::size_t alphabetSize, bool requireComplete) const {
        if (unusedCodes() == 0)
            return;
        if (requireComplete)
            throw InvalidGzip{};
        validateIncomplete(alphabetSize - counts[0]);
    }

    void validateIncomplete(std::size_t symbols) const {
        // Empty distance alphabets are legal until a match uses them.
        if (symbols == 0)
            return;
        if (symbols != 1)
            throw InvalidGzip{};
        // The only other permitted incomplete tree is a single one-bit symbol.
        if (counts[1] != 1)
            throw InvalidGzip{};
    }

    void buildTable(std::span<const unsigned> lengths) {
        std::array<unsigned, 16> offsets{};
        for (unsigned length = 1; length < 15; ++length)
            offsets[length + 1] = offsets[length] + counts[length];
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol)
            if (lengths[symbol])
                sorted[offsets[lengths[symbol]]++] = symbol;
    }

    std::array<unsigned, 16>  counts{};
    std::array<unsigned, 288> sorted{};
};

inline std::uint32_t gzipCrc(std::string_view bytes) {
    static constexpr auto table = [] {
        std::array<std::uint32_t, 256> result{};
        std::uint32_t                  index = 0;
        for (auto& entry : result) {
            auto value = index++;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
            entry = value;
        }
        return result;
    }();
    std::uint32_t crc = 0xffffffffu;
    for (char byte : bytes)
        crc = table[(crc ^ static_cast<unsigned char>(byte)) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

struct InflateContext {
    std::string& output;
    std::size_t  memberStart;
    std::size_t  limit;

    void requireCapacity(std::size_t count) const {
        if (count > limit - output.size())
            throw InvalidGzip{};
    }

    void appendLiteral(unsigned symbol) {
        requireCapacity(1);
        output.push_back(static_cast<char>(symbol));
    }

    void copyMatch(unsigned length, unsigned distance) {
        if (distance > output.size() - memberStart)
            throw InvalidGzip{};
        requireCapacity(length);
        const auto start = output.size();
        output.resize(start + length);
        // Forward copying preserves DEFLATE matches that overlap their own output.
        for (unsigned index = 0; index < length; ++index)
            output[start + index] = output[start + index - distance];
    }
};

inline unsigned readLength(GzipReader& input, unsigned symbol) {
    static constexpr std::array<unsigned, 29> lengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<unsigned, 29> lengthBits{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    if (symbol > 285)
        throw InvalidGzip{};
    return lengthBase[symbol - 257] + input.bits(lengthBits[symbol - 257]);
}

inline unsigned readDistance(GzipReader& input, const DeflateTree& distances) {
    static constexpr std::array<unsigned, 30> distanceBase{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<unsigned, 30> distanceBits{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    const auto                                symbol = distances.decode(input);
    if (symbol >= distanceBase.size())
        throw InvalidGzip{};
    return distanceBase[symbol] + input.bits(distanceBits[symbol]);
}

inline void inflateSymbol(GzipReader& input, InflateContext& context, unsigned symbol, const DeflateTree& distances) {
    if (symbol < 256) {
        context.appendLiteral(symbol);
        return;
    }
    const auto length   = readLength(input, symbol);
    const auto distance = readDistance(input, distances);
    context.copyMatch(length, distance);
}

inline void inflateBlock(GzipReader& input, InflateContext& context, const DeflateTree& literals, const DeflateTree& distances) {
    auto symbol = literals.decode(input);
    while (symbol != 256) {
        inflateSymbol(input, context, symbol, distances);
        symbol = literals.decode(input);
    }
}

inline void skipBytes(GzipReader& input, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
        input.bits(8);
}

inline void skipTerminatedString(GzipReader& input) {
    while (input.bits(8) != 0) {}
}

inline unsigned readHeaderPrefix(GzipReader& input) {
    constexpr std::array<unsigned, 3> magic{0x1f, 0x8b, 8};
    for (auto byte : magic)
        if (input.bits(8) != byte)
            throw InvalidGzip{};
    const auto flags = input.bits(8);
    if (flags & 0xe0)
        throw InvalidGzip{};
    skipBytes(input, 6);
    return flags;
}

inline void checkHeaderCrc(GzipReader& input, std::string_view header) {
    if (input.bits(16) != (gzipCrc(header) & 0xffff))
        throw InvalidGzip{};
}

inline void parseHeader(GzipReader& input, std::string_view bytes) {
    const auto start = input.offset();
    const auto flags = readHeaderPrefix(input);
    if (flags & 4)
        skipBytes(input, input.bits(16));
    if (flags & 8)
        skipTerminatedString(input);
    if (flags & 16)
        skipTerminatedString(input);
    if (flags & 2)
        checkHeaderCrc(input, bytes.substr(start, input.offset() - start));
}

inline void inflateStored(GzipReader& input, InflateContext& context) {
    input.align();
    const auto length = input.bits(16);
    if ((length ^ input.bits(16)) != 0xffff)
        throw InvalidGzip{};
    context.requireCapacity(length);
    context.output.append(input.readBytes(length));
}

inline void inflateFixed(GzipReader& input, InflateContext& context) {
    std::array<unsigned, 288> lengths{};
    lengths.fill(8);
    std::fill(lengths.begin() + 144, lengths.begin() + 256, 9);
    std::fill(lengths.begin() + 256, lengths.begin() + 280, 7);
    std::array<unsigned, 32> distances{};
    distances.fill(5);
    inflateBlock(input, context, DeflateTree(lengths), DeflateTree(distances));
}

inline DeflateTree readCodeTree(GzipReader& input, unsigned count) {
    static constexpr std::array<unsigned, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::array<unsigned, 19>                  codes{};
    for (unsigned index = 0; index < count; ++index)
        codes[order[index]] = input.bits(3);
    return DeflateTree(codes, true);
}

struct CodeLengthRun {
    unsigned count;
    unsigned value;
};

inline CodeLengthRun readPreviousRun(GzipReader& input, std::span<const unsigned> previous) {
    if (previous.empty())
        throw InvalidGzip{};
    return {3 + input.bits(2), previous.back()};
}

inline CodeLengthRun readZeroRun(GzipReader& input, unsigned symbol) {
    switch (symbol) {
        case 17:
            return {3 + input.bits(3), 0};
        case 18:
            return {11 + input.bits(7), 0};
        default:
            throw InvalidGzip{};
    }
}

inline CodeLengthRun readLengthRun(GzipReader& input, unsigned symbol, std::span<const unsigned> previous) {
    if (symbol < 16)
        return {1, symbol};
    if (symbol == 16)
        return readPreviousRun(input, previous);
    return readZeroRun(input, symbol);
}

inline void readCodeLengths(GzipReader& input, std::span<unsigned> lengths, const DeflateTree& codeTree) {
    for (std::size_t index = 0; index < lengths.size();) {
        const auto run = readLengthRun(input, codeTree.decode(input), lengths.first(index));
        if (run.count > lengths.size() - index)
            throw InvalidGzip{};
        std::fill_n(lengths.subspan(index).begin(), run.count, run.value);
        index += run.count;
    }
}

inline void inflateDynamic(GzipReader& input, InflateContext& context) {
    const auto literalCount  = input.bits(5) + 257;
    const auto distanceCount = input.bits(5) + 1;
    const auto codeCount     = input.bits(4) + 4;
    if (literalCount > 286)
        throw InvalidGzip{};
    if (distanceCount > 30)
        throw InvalidGzip{};
    const auto                codeTree = readCodeTree(input, codeCount);
    std::array<unsigned, 316> lengths{};
    readCodeLengths(input, std::span(lengths).first(literalCount + distanceCount), codeTree);
    if (lengths[256] == 0)
        throw InvalidGzip{};
    inflateBlock(input, context, DeflateTree(std::span(lengths).first(literalCount)), DeflateTree(std::span(lengths).subspan(literalCount, distanceCount)));
}

inline void inflateMember(GzipReader& input, InflateContext& context) {
    bool final = false;
    do {
        final = input.bits(1) != 0;
        switch (input.bits(2)) {
            case 0:
                inflateStored(input, context);
                break;
            case 1:
                inflateFixed(input, context);
                break;
            case 2:
                inflateDynamic(input, context);
                break;
            default:
                throw InvalidGzip{};
        }
    } while (!final);
}

inline std::uint32_t readUint32(GzipReader& input) {
    const auto low = input.bits(16);
    return low | (static_cast<std::uint32_t>(input.bits(16)) << 16);
}

inline void readTrailer(GzipReader& input, const InflateContext& context) {
    input.align();
    const auto crc  = readUint32(input);
    const auto size = readUint32(input);
    if (crc != gzipCrc(std::string_view(context.output).substr(context.memberStart)))
        throw InvalidGzip{};
    if (size != static_cast<std::uint32_t>(context.output.size() - context.memberStart))
        throw InvalidGzip{};
}

/** @brief Validates bounded gzip members; rejects malformed/truncated input and all trailing padding, including zeros. */
inline bool decodeGzipInto(std::string_view bytes, std::string& output, std::size_t limit) {
    output.clear();
    try {
        GzipReader input(bytes);
        do {
            parseHeader(input, bytes);
            InflateContext context{output, output.size(), limit};
            inflateMember(input, context);
            readTrailer(input, context);
        } while (!input.done());
        return true;
    } catch (const InvalidGzip&) {
        output.clear();
        return false;
    }
}

inline std::optional<std::string> decodeGzip(std::string_view bytes, std::size_t limit = defaultMaxDecodedAssetSize) {
    std::string output;
    if (!decodeGzipInto(bytes, output, limit))
        return std::nullopt;
    return output;
}

}  // namespace webfront::http::gzip
