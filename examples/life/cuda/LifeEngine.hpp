#pragma once

#include "LifeField.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace webfront::demo::lifecuda {

// The words of a field are sent over the bridge as their bytes, which are the packed layout only when the least
// significant byte comes first.
static_assert(std::endian::native == std::endian::little, "The Life field assumes a little-endian host");

enum class Backend : std::uint32_t { cpu = 0, cuda = 1 };
enum class Pattern : std::uint32_t { soup = 0, gliders = 1, acorn = 2, empty = 3 };

/// The name of the CUDA device, or an empty string when the build has no CUDA backend or no device answers.
[[nodiscard]] inline std::string cudaDevice() {
    std::array<char, 256> name{};
    return gpu::deviceName(name.data(), name.size()) ? std::string{name.data()} : std::string{};
}

/// A square Life torus and its generation counter. Not thread-safe: one owner advances it.
class Field {
public:
    /// @throws std::invalid_argument unless `length` (the side, in cells) is a positive multiple of 64.
    explicit Field(std::uint32_t length = fieldSide) : sideLength{checkedSide(length)}, cells(fieldWords(length)), scratch(cells.size()) {}

    [[nodiscard]] std::uint32_t side() const {
        return sideLength;
    }
    [[nodiscard]] std::uint32_t generation() const {
        return generationCount;
    }
    [[nodiscard]] bool alive(std::uint32_t x, std::uint32_t y) const {
        return (cells[wordOf(x, y)] >> (x % 64) & 1) != 0;
    }
    void set(std::uint32_t x, std::uint32_t y, bool value = true) {
        const auto mask = std::uint64_t{1} << (x % 64);
        auto&      word = cells[wordOf(x, y)];
        word            = value ? word | mask : word & ~mask;
    }
    [[nodiscard]] std::uint32_t population() const {
        std::uint32_t count = 0;
        for (const auto word : cells)
            count += static_cast<std::uint32_t>(std::popcount(word));
        return count;
    }

    /// The cells packed eight to a byte, cell `y * side + x` in bit `i % 8` of byte `i / 8`: the layout the
    /// bridge gives a std::bitset, received by JavaScript as a Uint8Array of side * side / 8 bytes.
    [[nodiscard]] std::vector<std::uint8_t> packed() const {
        std::vector<std::uint8_t> bytes(cells.size() * sizeof(std::uint64_t));
        std::memcpy(bytes.data(), cells.data(), bytes.size());
        return bytes;
    }

    /// Replaces the cells by a deterministic starting pattern and resets the generation counter.
    void seed(Pattern pattern, std::uint64_t seedValue = 1) {
        std::fill(cells.begin(), cells.end(), std::uint64_t{0});
        generationCount = 0;
        switch (pattern) {
        case Pattern::soup:
            // Three independent words per word of cells: a & (b | c) leaves three cells in eight alive.
            for (auto& word : cells) {
                const auto a = splitMix(seedValue), b = splitMix(seedValue), c = splitMix(seedValue);
                word         = a & (b | c);
            }
            break;
        case Pattern::gliders:
            for (std::uint32_t y = 0; y + 16 <= sideLength; y += 16)
                for (std::uint32_t x = 0; x + 16 <= sideLength; x += 16)
                    stamp(x + 6, y + 6, {{1, 0}, {2, 1}, {0, 2}, {1, 2}, {2, 2}});
            break;
        case Pattern::acorn:
            // A seven-cell methuselah: it grows for 5206 generations before it settles.
            stamp(sideLength / 2 - 3, sideLength / 2 - 1, {{1, 0}, {3, 1}, {0, 2}, {1, 2}, {4, 2}, {5, 2}, {6, 2}});
            break;
        case Pattern::empty:
            break;
        default:
            throw std::out_of_range("Life pattern");
        }
    }

    /// Computes `generations` generations with `backend`.
    /// @throws std::runtime_error when the CUDA backend is asked for and is unavailable or fails; the field is then
    ///         unchanged.
    void advance(Backend backend, std::uint32_t generations) {
        switch (backend) {
        case Backend::cpu:
            for (std::uint32_t i = 0; i < generations; ++i) {
                stepCpu(cells.data(), scratch.data(), sideLength);
                cells.swap(scratch);
            }
            break;
        case Backend::cuda: {
            if (!gpu::available())
                throw std::runtime_error("The CUDA backend is not available");
            std::array<char, 256> error{};
            scratch = cells;
            if (!gpu::advance(scratch.data(), sideLength, generations, error.data(), error.size()))
                throw std::runtime_error(std::string{"CUDA: "} + error.data());
            cells.swap(scratch);
            break;
        }
        default:
            throw std::out_of_range("Life backend");
        }
        generationCount += generations;
    }

    [[nodiscard]] bool operator==(const Field& other) const {
        return sideLength == other.sideLength && cells == other.cells;
    }

private:
    static std::uint32_t checkedSide(std::uint32_t length) {
        if (length == 0 || length % 64 != 0)
            throw std::invalid_argument("A Life field's side is a positive multiple of 64");
        return length;
    }
    [[nodiscard]] std::size_t wordOf(std::uint32_t x, std::uint32_t y) const {
        return static_cast<std::size_t>(y) * (sideLength / 64) + x / 64;
    }
    static std::uint64_t splitMix(std::uint64_t& state) {
        auto z = (state += 0x9e3779b97f4a7c15ull);
        z      = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z      = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    void stamp(std::uint32_t left, std::uint32_t top, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> shape) {
        for (const auto& [x, y] : shape)
            set((left + x) % sideLength, (top + y) % sideLength);
    }

    std::uint32_t              sideLength;
    std::vector<std::uint64_t> cells;
    std::vector<std::uint64_t> scratch;
    std::uint32_t              generationCount = 0;
};

}  // namespace webfront::demo::lifecuda
