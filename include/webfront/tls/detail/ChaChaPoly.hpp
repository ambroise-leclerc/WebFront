#pragma once
#include "Field.hpp"

#include <algorithm>
#include <bit>

namespace webfront::tls::detail {
inline std::uint32_t loadLittle(const Byte* data) {
    std::uint32_t word = 0;
    for (unsigned i = 0; i != 4; ++i)
        word |= std::uint32_t{data[i]} << (8 * i);
    return word;
}

using Nonce = std::array<Byte, 12>;
using Tag   = std::array<Byte, 16>;

inline std::array<Byte, 64> chachaBlock(const Digest& key, const Nonce& nonce, std::uint32_t counter) {
    // RFC 8439 section 2.3. The first four words encode the fixed ChaCha constant.
    std::array<std::uint32_t, 16> input{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (std::size_t i = 0; i != 8; ++i)
        input[4 + i] = loadLittle(key.data() + 4 * i);
    input[12] = counter;
    for (std::size_t i = 0; i != 3; ++i)
        input[13 + i] = loadLittle(nonce.data() + 4 * i);
    auto       state = input;
    const auto mix   = [&state](unsigned w, unsigned x, unsigned y, unsigned z) {
        state[w] += state[x];
        state[z]  = std::rotl(state[z] ^ state[w], 16);
        state[y] += state[z];
        state[x]  = std::rotl(state[x] ^ state[y], 12);
        state[w] += state[x];
        state[z]  = std::rotl(state[z] ^ state[w], 8);
        state[y] += state[z];
        state[x]  = std::rotl(state[x] ^ state[y], 7);
    };
    for (unsigned round = 0; round != 10; ++round) {
        for (unsigned i = 0; i != 4; ++i)
            mix(i, 4 + i, 8 + i, 12 + i);
        for (unsigned i = 0; i != 4; ++i)
            mix(i, 4 + (i + 1) % 4, 8 + (i + 2) % 4, 12 + (i + 3) % 4);
    }
    std::array<Byte, 64> stream;
    for (std::size_t i = 0; i != state.size(); ++i) {
        const auto word = state[i] + input[i];
        for (unsigned j = 0; j != 4; ++j)
            stream[4 * i + j] = static_cast<Byte>(word >> (8 * j));
    }
    wipe(input.data(), sizeof(input));
    wipe(state.data(), sizeof(state));
    return stream;
}

inline void chachaXor(const Digest& key, const Nonce& nonce, std::span<Byte> payload) {
    for (std::uint32_t counter = 1; !payload.empty(); ++counter) {
        auto       stream = chachaBlock(key, nonce, counter);
        const auto count  = std::min(payload.size(), stream.size());
        for (std::size_t i = 0; i != count; ++i)
            payload[i] ^= stream[i];
        wipe(stream.data(), stream.size());
        payload = payload.subspan(count);
    }
}

// RFC 8439 sections 2.5 and 2.8.1. The radix makes the carry from limb 4
// worth five in limb 0, since 2^130 is congruent to five modulo the prime.
inline Tag polyTag(const Digest& key, const Nonce& nonce, std::span<const Byte> aad, std::span<const Byte> ciphertext) {
    constexpr std::uint64_t low = (1u << 26) - 1;
    using Limbs                 = std::array<std::uint64_t, 5>;
    const auto unpack           = [](const Byte* data) -> Limbs {
        return {loadLittle(data) & low,
                (loadLittle(data + 3) >> 2) & low,
                (loadLittle(data + 6) >> 4) & low,
                (loadLittle(data + 9) >> 6) & low,
                loadLittle(data + 12) >> 8};
    };
    auto oneTimeKey = chachaBlock(key, nonce, 0);
    for (const auto i : {3u, 7u, 11u, 15u})
        oneTimeKey[i] &= 15;
    for (const auto i : {4u, 8u, 12u})
        oneTimeKey[i] &= 252;
    auto       r = unpack(oneTimeKey.data());
    Limbs      h{};
    const auto carry = [&h] {
        for (std::size_t i = 0; i != 4; ++i) {
            h[i + 1] += h[i] >> 26;
            h[i]     &= low;
        }
        h[0] += 5 * (h[4] >> 26);
        h[4] &= low;
    };
    const auto absorb = [&](const Byte* data) {
        const auto m = unpack(data);
        for (std::size_t i = 0; i != 5; ++i)
            h[i] += m[i];
        h[4] += 1u << 24;
        Limbs product{};
        for (std::size_t i = 0; i != 5; ++i)
            for (std::size_t j = 0; j != 5; ++j)
                product[(i + j) % 5] += h[i] * r[j] * (i + j < 5 ? 1u : 5u);
        h = product;
        carry();
        // The first carry leaves only limb 0 potentially wide.
        carry();
    };
    for (auto data : {aad, ciphertext}) {
        while (data.size() >= 16) {
            absorb(data.data());
            data = data.subspan(16);
        }
        if (!data.empty()) {
            Tag padded{};
            std::ranges::copy(data, padded.begin());
            absorb(padded.data());
        }
    }
    Tag lengths;
    for (unsigned i = 0; i != 8; ++i) {
        lengths[i]     = static_cast<Byte>(std::uint64_t{aad.size()} >> (8 * i));
        lengths[i + 8] = static_cast<Byte>(std::uint64_t{ciphertext.size()} >> (8 * i));
    }
    absorb(lengths.data());
    carry();

    // h + 5 crosses bit 130 precisely when h is at least the modulus.
    auto reduced = h;
    reduced[0]  += 5;
    for (std::size_t i = 0; i != 4; ++i) {
        reduced[i + 1] += reduced[i] >> 26;
        reduced[i]     &= low;
    }
    const auto mask = std::uint64_t{0} - (reduced[4] >> 26);
    for (std::size_t i = 0; i != 5; ++i)
        h[i] ^= (h[i] ^ reduced[i]) & mask;

    Tag      tag;
    unsigned padCarry = 0;
    for (unsigned i = 0; i != tag.size(); ++i) {
        const auto index = 8 * i / 26, offset = 8 * i % 26;
        const auto pair = h[index] | (index < 4 ? h[index + 1] << 26 : 0);
        padCarry       += static_cast<Byte>(pair >> offset) + unsigned{oneTimeKey[16 + i]};
        tag[i]          = static_cast<Byte>(padCarry);
        padCarry      >>= 8;
    }
    wipe(oneTimeKey.data(), oneTimeKey.size());
    wipe(r.data(), sizeof(r));
    wipe(h.data(), sizeof(h));
    wipe(reduced.data(), sizeof(reduced));
    return tag;
}
}  // namespace webfront::tls::detail
