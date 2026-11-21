#pragma once
#include "../../automation/Platform.hpp"
#include "ChaChaPoly.hpp"
#include "Curves.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace webfront::tls::detail {
using Bytes = std::vector<Byte>;
using View  = std::span<const Byte>;

inline View bytes(std::string_view string) {
    return {reinterpret_cast<const Byte*>(string.data()), string.size()};
}

inline void append(Bytes& out, View data) {
    out.insert(out.end(), data.begin(), data.end());
}
inline void put(Bytes& out, std::size_t value, unsigned count) {
    while (count)
        out.push_back(static_cast<Byte>(value >> (8 * --count)));
}

struct Sha256 {
    void update(View data) {
        length += data.size();
        while (!data.empty()) {
            const auto count = std::min(data.size(), block.size() - used);
            std::copy_n(data.begin(), count, block.begin() + used);
            used += count;
            data  = data.subspan(count);
            if (used == block.size()) {
                compress();
                used = 0;
            }
        }
    }

    Digest digest() const {
        auto                  copy = *this;
        std::array<Byte, 128> tail{0x80};
        const auto            count = (used < 56 ? 64u : 128u) - used;
        for (unsigned i = 0; i != 8; ++i)
            tail[count - 1 - i] = static_cast<Byte>((length * 8) >> (8 * i));
        copy.update(View(tail).first(count));
        Digest result;
        for (unsigned i = 0; i != result.size(); ++i)
            result[i] = static_cast<Byte>(copy.state[i / 4] >> (24 - 8 * (i % 4)));
        return result;
    }

    ~Sha256() {
        wipe(this, sizeof(*this));
    }

private:
    void compress() {
        static constexpr std::uint32_t constants[]{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
            0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
            0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
            0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        auto [a, b, c, d, e, f, g, h] = state;
        std::array<std::uint32_t, 16> words;
        for (unsigned i = 0; i != 64; ++i) {
            auto& word = words[i % 16];
            if (i < 16) {
                word = 0;
                for (unsigned j = 0; j != 4; ++j)
                    word = (word << 8) | block[4 * i + j];
            } else {
                const auto x = words[(i - 15) % 16], y = words[(i - 2) % 16];
                word += words[(i - 7) % 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
            }
            const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + constants[i] + word;
            const auto t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h             = g;
            g             = f;
            f             = e;
            e             = d + t1;
            d             = c;
            c             = b;
            b             = a;
            a             = t1 + t2;
        }
        const std::array final{a, b, c, d, e, f, g, h};
        for (unsigned i = 0; i != state.size(); ++i)
            state[i] += final[i];
        wipe(words.data(), sizeof(words));
    }

    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<Byte, 64>         block{};
    std::uint64_t                length = 0;
    std::size_t                  used   = 0;
};

inline Digest hash(View data = {}) {
    Sha256 sha;
    sha.update(data);
    return sha.digest();
}

// All HMAC keys used by the TLS suite and RFC 6979 fit in a single SHA-256 block.
inline Digest hmac(View key, std::initializer_list<View> chunks) {
    if (key.size() > 64)
        throw std::invalid_argument("HMAC key exceeds one SHA-256 block");
    std::array<Byte, 64> pad;
    pad.fill(0x36);
    for (std::size_t i = 0; i != key.size(); ++i)
        pad[i] ^= key[i];
    Sha256 inner, outer;
    inner.update(pad);
    for (const auto chunk : chunks)
        inner.update(chunk);
    for (auto& byte : pad)
        byte ^= 0x36 ^ 0x5c;
    outer.update(pad);
    auto digest = inner.digest();
    outer.update(digest);
    wipe(pad.data(), pad.size());
    wipe(digest.data(), digest.size());
    return outer.digest();
}

inline Digest expandLabel(View key, std::string_view label, View context = {}, unsigned size = 32) {
    Bytes info;
    put(info, size, 2);
    put(info, 6 + label.size(), 1);
    append(info, bytes("tls13 "));
    append(info, bytes(label));
    put(info, context.size(), 1);
    append(info, context);
    info.push_back(1);  // HKDF block number. TLS here needs at most one block.
    return hmac(key, {info});
}

inline void entropy(std::span<Byte> out) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
#else
    if (getentropy(out.data(), out.size()) != 0)
#endif
        throw std::runtime_error("TLS: operating system entropy unavailable");
}

template <std::size_t Size = 32>
struct Secret : std::array<Byte, Size> {
    ~Secret() {
        wipe(this->data(), Size);
    }
};

inline Bytes sign(std::span<const Byte, 32> key, const Digest& messageHash) {
    using F     = P256Order;
    auto scalar = decode(key);
    struct Erase {
        Integer& value;
        ~Erase() {
            wipe(value.data(), sizeof(value));
        }
    } erase{scalar};
    if (!F::validScalar(scalar))
        throw std::invalid_argument("TLS: invalid P-256 scalar");
    scalar             = F::enter(scalar);
    const auto reduced = encode(F::reduce(decode(messageHash)));
    const auto z       = F::enter(decode(reduced));
    Secret<>   v, mac{};
    v.fill(1);
    const std::array<Byte, 1> zero{0}, one{1};
    mac = {hmac(mac, {v, zero, key, reduced})};
    v   = {hmac(mac, {v})};
    mac = {hmac(mac, {v, one, key, reduced})};
    v   = {hmac(mac, {v})};
    for (;;) {
        v       = {hmac(mac, {v})};
        auto  k = decode(v);
        Erase eraseNonce{k};
        if (F::validScalar(k)) {
            const auto x   = F::reduce(p256Base(k)[0]);
            const auto sum = F::add(z, F::multiply(F::enter(x), scalar));
            const auto y   = F::leave(F::multiply(F::invert(F::enter(k)), sum));
            if (nonzero(x) && nonzero(y)) {
                const auto r = encode(x), s = encode(y);
                Bytes      signature{0x30, 0};
                for (View value : {View(r), View(s)}) {
                    while (value.size() > 1 && value.front() == 0)
                        value = value.subspan(1);
                    const bool prefix = value.front() >= 128;
                    put(signature, 2, 1);
                    put(signature, value.size() + prefix, 1);
                    if (prefix)
                        signature.push_back(0);
                    append(signature, value);
                }
                signature[1] = static_cast<Byte>(signature.size() - 2);
                return signature;
            }
        }
        mac = {hmac(mac, {v, zero})};
        v   = {hmac(mac, {v})};
    }
}
}  // namespace webfront::tls::detail
