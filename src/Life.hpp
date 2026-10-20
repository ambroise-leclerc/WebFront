#pragma once

#include <bitset>
#include <stdexcept>
#include <string>

namespace webfront::demo {
using LifeGrid = std::bitset<32 * 32>;

// B3/S23 on a torus: compute a new generation without modifying the old one.
inline LifeGrid nextLife(const LifeGrid& cells) {
    LifeGrid next;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            int neighbors = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy)
                        neighbors += cells[static_cast<std::size_t>(((y + dy + 32) % 32) * 32 + (x + dx + 32) % 32)];
            const auto i = static_cast<std::size_t>(y * 32 + x);
            next[i]      = neighbors == 3 || (cells[i] && neighbors == 2);
        }
    return next;
}

// The bridge carries a bounded binary string; std::bitset itself is not a wire type.
inline std::string lifeStep(const std::string& cells) {
    if (cells.size() != 32 * 32 || cells.find_first_not_of("01") != std::string::npos)
        throw std::invalid_argument("Expected a 32 by 32 binary grid");
    return nextLife(LifeGrid{cells}).to_string();
}
}  // namespace webfront::demo
