// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-21
//
// WebFront — tls
// Role: Finite field arithmetic and secret-safe comparison and wiping helpers for the TLS curves.
// Thread: bridge-io | Alloc: zero-hot-path
// LLM: cryptographic primitive; keep constant-time behavior, verify with test vectors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace webfront::tls::detail {
using Byte   = std::uint8_t;
using Digest = std::array<Byte, 32>;

inline void wipe(void* data, std::size_t size) {
    auto* out = static_cast<volatile Byte*>(data);
    while (size--)
        *out++ = 0;
}

inline bool equalSecret(std::span<const Byte> a, std::span<const Byte> b) {
    if (a.size() != b.size())
        return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i != a.size(); ++i)
        difference |= unsigned{a[i]} ^ b[i];
    return difference == 0;
}

using Integer = std::array<std::uint32_t, 8>;

inline Integer decode(std::span<const Byte, 32> bytes, bool littleEndian = false) {
    Integer result{};
    for (std::size_t i = 0; i != bytes.size(); ++i)
        result[i / 4] |= std::uint32_t{bytes[littleEndian ? i : 31 - i]} << (8 * (i % 4));
    return result;
}

inline Digest encode(const Integer& value, bool littleEndian = false) {
    Digest result;
    for (std::size_t i = 0; i != result.size(); ++i)
        result[littleEndian ? i : 31 - i] = static_cast<Byte>(value[i / 4] >> (8 * (i % 4)));
    return result;
}

inline std::uint32_t bit(const Integer& value, unsigned index) {
    return (value[index / 32] >> (index % 32)) & 1u;
}

inline std::uint32_t nonzero(const Integer& value) {
    std::uint32_t combined = 0;
    for (auto word : value)
        combined |= word;
    return (combined | (0u - combined)) >> 31;
}

constexpr std::uint32_t selectionMask(std::uint32_t choice) {
    if consteval {
        return 0u - choice;
    } else {
        // Keep the mask opaque: Clang can otherwise select an input pointer and
        // read through it, making the memory address depend on the secret bit.
        const volatile std::uint32_t mask = 0u - choice;
        return mask;
    }
}

constexpr Integer select(const Integer& first, const Integer& second, std::uint32_t choice) {
    const auto mask = selectionMask(choice);
    Integer    result;
    for (std::size_t i = 0; i != result.size(); ++i)
        result[i] = first[i] ^ ((first[i] ^ second[i]) & mask);
    return result;
}

// Eight little-endian limbs. Field values are in Montgomery form between import and export.
template <Integer Modulus>
struct Field {
    static constexpr std::uint32_t subtract(Integer& out, const Integer& a, const Integer& b) {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i != out.size(); ++i) {
            const auto difference = std::uint64_t{a[i]} - b[i] - borrow;
            out[i]                = static_cast<std::uint32_t>(difference);
            borrow                = difference >> 63;
        }
        return static_cast<std::uint32_t>(borrow);
    }

    static constexpr Integer reduce(const Integer& a, std::uint32_t high = 0) {
        Integer    reduced;
        const auto borrow = subtract(reduced, a, Modulus);
        return select(reduced, a, borrow & (1u ^ high));
    }

    static constexpr Integer add(const Integer& a, const Integer& b) {
        Integer       sum;
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i != sum.size(); ++i) {
            carry  += std::uint64_t{a[i]} + b[i];
            sum[i]  = static_cast<std::uint32_t>(carry);
            carry >>= 32;
        }
        return reduce(sum, static_cast<std::uint32_t>(carry));
    }

    static Integer sub(const Integer& a, const Integer& b) {
        Integer       difference;
        const auto    mask  = 0u - subtract(difference, a, b);
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i != difference.size(); ++i) {
            carry        += std::uint64_t{difference[i]} + (Modulus[i] & mask);
            difference[i] = static_cast<std::uint32_t>(carry);
            carry       >>= 32;
        }
        return difference;
    }

    static Integer multiply(const Integer& a, const Integer& b) {
        constexpr std::size_t                limbs = Modulus.size();
        std::array<std::uint32_t, limbs + 2> work{};
        for (const auto digit : a) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j != limbs; ++j) {
                carry  += std::uint64_t{digit} * b[j] + work[j];
                work[j] = static_cast<std::uint32_t>(carry);
                carry >>= 32;
            }
            carry          += work[limbs];
            work[limbs]     = static_cast<std::uint32_t>(carry);
            work[limbs + 1] = static_cast<std::uint32_t>(carry >> 32);

            // Cancel the low limb by adding a multiple of the modulus, then divide by 2^32.
            const auto multiple = work[0] * negativeInverse;
            carry               = (std::uint64_t{multiple} * Modulus[0] + work[0]) >> 32;
            for (std::size_t j = 1; j != limbs; ++j) {
                carry      += std::uint64_t{multiple} * Modulus[j] + work[j];
                work[j - 1] = static_cast<std::uint32_t>(carry);
                carry     >>= 32;
            }
            carry          += work[limbs];
            work[limbs - 1] = static_cast<std::uint32_t>(carry);
            work[limbs]     = work[limbs + 1] + static_cast<std::uint32_t>(carry >> 32);
        }
        Integer result;
        for (std::size_t i = 0; i != limbs; ++i)
            result[i] = work[i];
        return reduce(result, work[limbs]);
    }

    static Integer square(const Integer& a) {
        return multiply(a, a);
    }
    static Integer enter(const Integer& a) {
        return multiply(a, radixSquared);
    }
    static Integer leave(const Integer& a) {
        return multiply(a, {1});
    }

    static Integer invert(const Integer& a) {
        // Fermat's little theorem. The exponent depends only on the public modulus.
        constexpr auto exponent = [] {
            auto value = Modulus;
            value[0]  -= 2;
            return value;
        }();
        auto power = enter({1});
        for (unsigned i = 256; i-- != 0;) {
            power = square(power);
            if (bit(exponent, i))
                power = multiply(power, a);
        }
        return power;
    }

    static bool validScalar(const Integer& value) {
        Integer ignored;
        return (subtract(ignored, value, Modulus) & nonzero(value)) != 0;
    }

private:
    static constexpr auto negativeInverse = [] {
        std::uint32_t inverse = 1;
        for (unsigned i = 0; i != 5; ++i)
            inverse *= 2u - Modulus[0] * inverse;
        return 0u - inverse;
    }();

    static constexpr auto radixSquared = [] {
        Integer n{1};
        for (unsigned i = 0; i != 512; ++i)
            n = add(n, n);
        return n;
    }();
};
}  // namespace webfront::tls::detail
