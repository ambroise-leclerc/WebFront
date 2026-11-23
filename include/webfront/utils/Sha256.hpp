/// @brief SHA-256 (FIPS 180-4), computed incrementally: a file is digested block by block, without loading it whole.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace webfront::utils {

class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    /// Adds `bytes` to the message. Whole blocks are compressed where they lie; only a partial one is copied.
    void update(std::span<const std::byte> bytes) {
        length          += bytes.size();
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
        std::size_t size = bytes.size();
        if (filled != 0) {
            const auto taken = std::min(size, block.size() - filled);
            std::memcpy(block.data() + filled, data, taken);
            filled += taken;
            data   += taken;
            size   -= taken;
            if (filled < block.size())
                return;
            compress(block.data());
            filled = 0;
        }
        for (; size >= block.size(); data += block.size(), size -= block.size())
            compress(data);
        if (size != 0) {
            std::memcpy(block.data(), data, size);
            filled = size;
        }
    }
    void update(std::string_view text) {
        update(std::as_bytes(std::span(text.data(), text.size())));
    }

    /// The digest of everything added. The object is then spent.
    Digest finish() {
        const auto bits = static_cast<std::uint64_t>(length) * 8;
        // `filled` is below 64 between calls; masking it lets static analysis see that every index stays in the block.
        std::size_t at = filled & (block.size() - 1);
        block[at++]    = 0x80;
        if (at > 56) {
            std::fill(block.begin() + static_cast<std::ptrdiff_t>(at), block.end(), std::uint8_t{0});
            compress(block.data());
            at = 0;
        }
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(at), block.begin() + 56, std::uint8_t{0});
        for (std::size_t i = 0; i < 8; ++i)
            block[56 + i] = static_cast<std::uint8_t>((bits >> (56 - 8 * i)) & 255);
        filled = 0;
        compress(block.data());
        Digest digest{};
        for (std::size_t i = 0; i < state.size(); ++i)
            for (std::size_t j = 0; j < 4; ++j)
                digest[i * 4 + j] = static_cast<std::uint8_t>((state[i] >> (24 - 8 * j)) & 255);
        return digest;
    }

    /// Lowercase hexadecimal of a digest.
    static std::string hex(const Digest& digest) {
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string                       text;
        text.reserve(digest.size() * 2);
        for (const auto byte : digest) {
            text += digits[byte >> 4];
            text += digits[byte & 15];
        }
        return text;
    }

private:
    // Compresses the 64 bytes at `input` into the state.
    void compress(const std::uint8_t* input) {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i)
            words[i] = (std::uint32_t{input[i * 4]} << 24) | (std::uint32_t{input[i * 4 + 1]} << 16) | (std::uint32_t{input[i * 4 + 2]} << 8)
                       | std::uint32_t{input[i * 4 + 3]};
        for (std::size_t i = 16; i < 64; ++i) {
            const auto x = words[i - 15], y = words[i - 2];
            words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + words[i - 7] + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
        }
        auto [a, b, c, d, e, f, g, h] = state;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto first  = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + constants[i] + words[i];
            const auto second = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h                 = g;
            g                 = f;
            f                 = e;
            e                 = d + first;
            d                 = c;
            c                 = b;
            b                 = a;
            a                 = first + second;
        }
        const std::array values{a, b, c, d, e, f, g, h};
        for (std::size_t i = 0; i < state.size(); ++i)
            state[i] += values[i];
    }

    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> block{};
    std::size_t                  filled = 0;
    std::uint64_t                length = 0;
};

/// The 32 raw bytes of the SHA-256 of `input`.
inline std::string sha256(std::string_view input) {
    Sha256 hasher;
    hasher.update(input);
    const auto digest = hasher.finish();
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

}  // namespace webfront::utils
