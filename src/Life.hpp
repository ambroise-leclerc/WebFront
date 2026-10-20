#pragma once

#include <bitset>

namespace webfront::demo {
using LifeGrid = std::bitset<32 * 32>;

namespace detail {
inline std::size_t lifeIndex(int x, int y) {
    return static_cast<std::size_t>(((y + 32) % 32) * 32 + (x + 32) % 32);
}

inline int liveNeighbors(const LifeGrid& cells, int x, int y) {
    int neighbors = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy)
                neighbors += cells[lifeIndex(x + dx, y + dy)];
    return neighbors;
}
}  // namespace detail

// B3/S23 on a torus: compute a new generation without modifying the old one.
inline LifeGrid nextLife(const LifeGrid& cells) {
    LifeGrid next;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const auto neighbors = detail::liveNeighbors(cells, x, y);
            const auto i         = detail::lifeIndex(x, y);
            next[i]              = neighbors == 3 || (cells[i] && neighbors == 2);
        }
    return next;
}

}  // namespace webfront::demo
