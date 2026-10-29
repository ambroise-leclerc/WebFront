/// @brief OS entropy and shared credential comparison, independent of session/transport ownership.
#pragma once
#include "Platform.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace webfront::automation::channel {
inline std::string nonce() {
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Cannot generate attachment capability");
#else
    if (getentropy(bytes.data(), bytes.size()))
        throw std::runtime_error("Cannot generate attachment capability");
#endif
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                result;
    for (auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
inline bool sameToken(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    unsigned mismatch = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        mismatch |= static_cast<unsigned>(static_cast<unsigned char>(left[index])) ^ static_cast<unsigned>(static_cast<unsigned char>(right[index]));
    return mismatch == 0;
}

}  // namespace webfront::automation::channel
