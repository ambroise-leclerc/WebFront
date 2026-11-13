#pragma once

// The Life kernel shared by the CPU and the CUDA backends. This header is compiled by nvcc too, whose host
// language stops at C++20: it stays plain C++17 and includes no WebFront header.

#include <cstddef>
#include <cstdint>

#if defined(__CUDACC__)
    #define WEBFRONT_LIFE_KERNEL __host__ __device__
#else
    #define WEBFRONT_LIFE_KERNEL
#endif

namespace webfront::demo::lifecuda {

/// Side of the demo's square torus, in cells. A field packs 64 cells per word: cell (x, y) is bit `x % 64` of
/// word `y * (side / 64) + x / 64`. On a little-endian host the bytes of the words are therefore the bridge's
/// packed layout (cell `i` is bit `i % 8` of byte `i / 8`).
inline constexpr std::uint32_t fieldSide = 2048;

/// Number of 64-bit words of a `side` x `side` field. `side` must be a positive multiple of 64.
constexpr std::size_t fieldWords(std::uint32_t side) {
    return static_cast<std::size_t>(side) * (side / 64);
}

/// The next generation (B3/S23, both axes wrapping) of the 64 cells of one word, computed for all of them at
/// once: the eight neighbours are summed by bit-sliced adders instead of being counted cell by cell.
WEBFRONT_LIFE_KERNEL inline std::uint64_t nextWord(const std::uint64_t* cells, std::uint32_t side, std::uint32_t row, std::uint32_t word) {
    const std::uint32_t  words = side / 64;
    const std::uint32_t  west  = (word + words - 1) % words;
    const std::uint32_t  east  = (word + 1) % words;
    const std::uint64_t* up    = cells + static_cast<std::size_t>((row + side - 1) % side) * words;
    const std::uint64_t* mid   = cells + static_cast<std::size_t>(row) * words;
    const std::uint64_t* down  = cells + static_cast<std::size_t>((row + 1) % side) * words;

    // The western neighbour of bit x is bit x - 1, borrowed from the previous word at x = 0 (itself when the row
    // is one word wide, which makes the shift a rotation).
    const std::uint64_t upWest   = (up[word] << 1) | (up[west] >> 63);
    const std::uint64_t upEast   = (up[word] >> 1) | (up[east] << 63);
    const std::uint64_t midWest  = (mid[word] << 1) | (mid[west] >> 63);
    const std::uint64_t midEast  = (mid[word] >> 1) | (mid[east] << 63);
    const std::uint64_t downWest = (down[word] << 1) | (down[west] >> 63);
    const std::uint64_t downEast = (down[word] >> 1) | (down[east] << 63);

    // Per row, the number of living neighbours as two bit planes (0 to 3 above and below, 0 to 2 beside).
    const std::uint64_t upLow    = upWest ^ up[word] ^ upEast;
    const std::uint64_t upHigh   = (upWest & up[word]) | (upEast & (upWest ^ up[word]));
    const std::uint64_t midLow   = midWest ^ midEast;
    const std::uint64_t midHigh  = midWest & midEast;
    const std::uint64_t downLow  = downWest ^ down[word] ^ downEast;
    const std::uint64_t downHigh = (downWest & down[word]) | (downEast & (downWest ^ down[word]));

    // Total = ones + 2 * (number of set planes among carry, upHigh, midHigh, downHigh). It is 2 or 3 exactly
    // when one of those four planes is set; `ones` then tells 3 from 2.
    const std::uint64_t ones       = upLow ^ midLow ^ downLow;
    const std::uint64_t carry      = (upLow & midLow) | (downLow & (upLow ^ midLow));
    const std::uint64_t exactlyOne = ((carry ^ upHigh) ^ (midHigh ^ downHigh)) & ~((carry & upHigh) | (midHigh & downHigh));
    return exactlyOne & (ones | mid[word]);
}

/// One generation on the CPU, single-threaded. `in` and `out` hold fieldWords(side) words and must not overlap.
inline void stepCpu(const std::uint64_t* in, std::uint64_t* out, std::uint32_t side) {
    const std::uint32_t words = side / 64;
    for (std::uint32_t row = 0; row < side; ++row)
        for (std::uint32_t word = 0; word < words; ++word)
            out[static_cast<std::size_t>(row) * words + word] = nextWord(in, side, row, word);
}

/// The CUDA backend, defined in LifeField.cu when the build found a CUDA toolkit (WEBFRONT_LIFE_CUDA), and
/// reporting itself unavailable otherwise. The interface is plain data so that the two translation units, built
/// by different compilers at different language levels, share nothing but these signatures.
namespace gpu {
#if defined(WEBFRONT_LIFE_CUDA)
    /// True when a CUDA device answers. Never throws; safe from any thread.
    bool available() noexcept;
    /// Copies the device's name, truncated and NUL-terminated, into `name`. False without a device.
    bool deviceName(char* name, std::size_t size) noexcept;
    /// Advances `cells` (fieldWords(side) words) by `generations` in place: one upload, the generations on the
    /// device, one download. On failure returns false, writes the reason into `error` and leaves `cells` unspecified.
    bool advance(std::uint64_t* cells, std::uint32_t side, std::uint32_t generations, char* error, std::size_t errorSize) noexcept;
    /// Frees the device buffers kept between calls. Call it before the process exits.
    void release() noexcept;
#else
    inline bool available() noexcept {
        return false;
    }
    inline bool deviceName(char*, std::size_t) noexcept {
        return false;
    }
    inline bool advance(std::uint64_t*, std::uint32_t, std::uint32_t, char* error, std::size_t errorSize) noexcept {
        if (errorSize != 0)
            *error = '\0';
        return false;
    }
    inline void release() noexcept {}
#endif
}  // namespace gpu

}  // namespace webfront::demo::lifecuda
