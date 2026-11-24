// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-29
//
// WebFront — automation
// Role: Authenticates page callbacks without putting a reusable secret on their wire messages (HMAC over a sequence).
// Thread: bridge-io | Alloc: heap-init
// LLM: security sensitive; read ADR-025 before changing the payload layout

#pragma once
#include "../utils/Sha256.hpp"
#include "Channel.hpp"

#include <bit>
#include <limits>

namespace webfront::automation::authentication {
using utils::sha256;  // 32 raw bytes, FIPS 180-4
// RFC 2104 construction; RFC 4231 vectors cover padding, binary keys and long keys/messages.
inline std::string hmac(std::string_view key, std::string_view message) {
    const std::string normalized = key.size() > 64 ? sha256(key) : std::string(key);
    std::string       inner(64, '\x36'), outer(64, '\x5c');
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const auto byte = static_cast<unsigned char>(normalized[i]);
        inner[i]        = static_cast<char>(byte ^ 0x36U);
        outer[i]        = static_cast<char>(byte ^ 0x5cU);
    }
    const auto                 digest = sha256(outer + sha256(inner + std::string(message)));
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                hex;
    for (const char value : digest) {
        const auto byte = static_cast<unsigned char>(value);
        hex            += digits[byte >> 4];
        hex            += digits[byte & 15];
    }
    return hex;
}
inline std::string payload(std::string_view sequence, std::string_view method, std::initializer_list<std::string_view> arguments) {
    std::string result = "webfront.automation.v1\n" + std::string(sequence) + "\n" + std::string(method) + "\n";
    for (const auto argument : arguments)
        result += std::to_string(argument.size()) + ":" + std::string(argument);
    return result;
}
class PageAuthentication {
public:
    explicit PageAuthentication(std::string secret = channel::nonce()) : key(std::move(secret)) { }
    const std::string& secret() const {
        return key;
    }
    void authorize(std::string_view sequence, std::string_view signature, std::string_view method, std::initializer_list<std::string_view> arguments) {
        std::uint64_t value     = 0;
        const auto [end, error] = std::from_chars(sequence.data(), sequence.data() + sequence.size(), value);
        if (error != std::errc{} || end != sequence.data() + sequence.size() || sequence != std::to_string(value) || value <= counter
            || !channel::sameToken(signature, hmac(key, payload(sequence, method, arguments))))
            throw std::runtime_error("Automation callback authentication failed");
        counter = value;
    }

private:
    std::string   key;
    std::uint64_t counter = 0;
};
}  // namespace webfront::automation::authentication
