// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-01-31
//
// WebFront — compatibility
// Role: C++23 library facilities that selected targets lack.
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: compatibility shim; remove entries as toolchains catch up

#pragma once
#include <bit>
#include <cstdint>
#include <future>
#include <type_traits>
namespace std {

/// template<typename T> constexpr T byteswap( T n ) noexcept;
/// Reverses the bytes in the given integer value n.
#ifndef __cpp_lib_byteswap
    #if defined(_MSC_VER)
inline auto byteswap(uint64_t v) noexcept {
    return _byteswap_uint64(v);
}
inline auto byteswap(uint32_t v) noexcept {
    return _byteswap_ulong(v);
}
inline auto byteswap(uint16_t v) noexcept {
    return _byteswap_ushort(v);
}
    #else
constexpr auto byteswap(uint64_t v) noexcept {
    return __builtin_bswap64(v);
}
constexpr auto byteswap(uint32_t v) noexcept {
    return __builtin_bswap32(v);
}
constexpr auto byteswap(uint16_t v) noexcept {
    return __builtin_bswap16(v);
}
    #endif
#endif

}  // namespace std
