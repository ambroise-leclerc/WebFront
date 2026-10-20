#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace webfront::demo {

namespace detail {
// One spigot step over the remainders: returns the next candidate digit (0..10).
inline int spigotStep(std::vector<int>& remainder) {
    int quotient = 0;
    for (auto i = static_cast<int>(remainder.size()); i > 0; --i) {
        auto&     cell  = remainder[static_cast<std::size_t>(i - 1)];
        const int value = 10 * cell + quotient * i;
        cell            = value % (2 * i - 1);
        quotient        = value / (2 * i - 1);
    }
    remainder[0] = quotient % 10;
    return quotient / 10;
}

// The digits, with a leading placeholder, holding back runs of nines until a carry settles them.
inline std::string spigotDigits(int count) {
    std::vector<int> remainder(static_cast<std::size_t>(count * 10 / 3 + 1), 2);
    std::string      digits;
    int              previous = 0, nines = 0;
    for (int digit = 0; digit < count; ++digit) {
        const int quotient = spigotStep(remainder);
        if (quotient == 9) {
            ++nines;
            continue;
        }
        const int carry = quotient == 10;
        digits += static_cast<char>('0' + previous + carry);
        digits.append(static_cast<std::size_t>(nines), "90"[carry]);
        previous = quotient % 10;
        nines    = 0;
    }
    return digits + static_cast<char>('0' + previous);
}
}  // namespace detail

// Decimal spigot; deliberately small and bounded so it is suitable for the introductory demo.
inline std::string pi(std::uint32_t decimals) {
    if (decimals > 1000)
        throw std::invalid_argument("Choose at most 1000 decimals");
    auto result = detail::spigotDigits(static_cast<int>(decimals) + 3).substr(1, static_cast<std::size_t>(decimals) + 1);
    if (decimals != 0)
        result.insert(1, ".");
    return result;
}

}  // namespace webfront::demo
