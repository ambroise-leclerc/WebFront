/// @brief Authenticate page callbacks without putting a reusable secret on their wire messages.
#pragma once
#include "Channel.hpp"

#include <bit>
#include <limits>

namespace webfront::automation::authentication {
// SHA-256 (FIPS 180-4), used only by the optional page authenticator. Returns 32 raw bytes.
inline std::string sha256(std::string_view input) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string                  padded{input};
    padded += static_cast<char>(0x80);
    while (padded.size() % 64 != 56)
        padded += '\0';
    const auto bits = static_cast<std::uint64_t>(input.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8)
        padded += static_cast<char>((bits >> shift) & 255);
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i)
            for (std::size_t j = 0; j < 4; ++j)
                words[i] = (words[i] << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(padded[offset + i * 4 + j]));
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
    std::string digest;
    for (const auto word : state)
        for (int shift = 24; shift >= 0; shift -= 8)
            digest += static_cast<char>((word >> shift) & 255);
    return digest;
}
// RFC 2104 construction; RFC 4231 vectors cover padding, binary keys and long keys/messages.
inline std::string hmac(std::string_view key, std::string_view message) {
    const std::string normalized = key.size() > 64 ? sha256(key) : std::string(key);
    std::string       inner(64, '\x36'), outer(64, '\x5c');
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const auto byte = static_cast<unsigned char>(normalized[i]);
        inner[i]        = static_cast<char>(byte ^ 0x36U);
        outer[i]        = static_cast<char>(byte ^ 0x5cU);
    }
    const auto                 digest = sha256(outer + sha256(inner + std::string(message)));
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                hex;
    for (const char value : digest) {
        const auto byte = static_cast<unsigned char>(value);
        hex            += digits[byte >> 4];
        hex            += digits[byte & 15];
    }
    return hex;
}
inline std::string payload(std::string_view sequence, std::string_view method, std::initializer_list<std::string_view> arguments) {
    std::string result = "webfront.automation.v1\n" + std::string(sequence) + "\n" + std::string(method) + "\n";
    for (const auto argument : arguments)
        result += std::to_string(argument.size()) + ":" + std::string(argument);
    return result;
}
class PageAuthentication {
public:
    explicit PageAuthentication(std::string secret = channel::nonce()) : key(std::move(secret)) {}
    const std::string& secret() const {
        return key;
    }
    void authorize(std::string_view sequence, std::string_view signature, std::string_view method, std::initializer_list<std::string_view> arguments) {
        std::uint64_t value     = 0;
        const auto [end, error] = std::from_chars(sequence.data(), sequence.data() + sequence.size(), value);
        if (error != std::errc{} || end != sequence.data() + sequence.size() || sequence != std::to_string(value) || value <= counter
            || !channel::sameToken(signature, hmac(key, payload(sequence, method, arguments))))
            throw std::runtime_error("Automation callback authentication failed");
        counter = value;
    }

private:
    std::string   key;
    std::uint64_t counter = 0;
};
}  // namespace webfront::automation::authentication
