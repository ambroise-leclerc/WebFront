/// @file C++20Support.hpp
/// @date 31/01/2022 10:17:42
/// @author Ambroise Leclerc
/// @brief C++23 missing functionnalities for selected targets
#pragma once
#include <bit>
namespace std {


/// template<typename T> constexpr T byteswap( T n ) noexcept;
/// Reverses the bytes in the given integer value n. 
#ifndef __cpp_lib_byteswap
#if defined(_MSC_VER)
inline auto byteswap(uint64_t v) noexcept { return _byteswap_uint64(v); }
inline auto byteswap(uint32_t v) noexcept { return _byteswap_ulong(v); }
inline auto byteswap(uint16_t v) noexcept { return _byteswap_ushort(v); }
#else
constexpr auto byteswap(uint64_t v) noexcept { return __builtin_bswap64(v); }
constexpr auto byteswap(uint32_t v) noexcept { return __builtin_bswap32(v); }
constexpr auto byteswap(uint16_t v) noexcept { return __builtin_bswap16(v); }
#endif
#endif

} // namespace std