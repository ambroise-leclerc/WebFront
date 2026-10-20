#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace webfront::demo {

// Decimal spigot; deliberately small and bounded so it is suitable for the introductory demo.
inline std::string pi(std::uint32_t decimals) {
    if (decimals > 1000)
        throw std::invalid_argument("Choose at most 1000 decimals");
    const auto       count = static_cast<int>(decimals) + 3;
    std::vector<int> remainder(static_cast<std::size_t>(count * 10 / 3 + 1), 2);
    std::string      digits;
    int              previous = 0, nines = 0;
    for (int digit = 0; digit < count; ++digit) {
        int quotient = 0;
        for (int i = static_cast<int>(remainder.size()); i > 0; --i) {
            const int value                            = 10 * remainder[static_cast<std::size_t>(i - 1)] + quotient * i;
            remainder[static_cast<std::size_t>(i - 1)] = value % (2 * i - 1);
            quotient                                   = value / (2 * i - 1);
        }
        remainder[0] = quotient % 10;
        quotient    /= 10;
        if (quotient == 9)
            ++nines;
        else {
            digits += static_cast<char>('0' + previous + (quotient == 10 ? 1 : 0));
            digits.append(static_cast<std::size_t>(nines), quotient == 10 ? '0' : '9');
            previous = quotient == 10 ? 0 : quotient;
            nines    = 0;
        }
    }
    digits     += static_cast<char>('0' + previous);
    auto result = digits.substr(1, static_cast<std::size_t>(decimals) + 1);
    if (decimals != 0)
        result.insert(1, ".");
    return result;
}

}  // namespace webfront::demo
