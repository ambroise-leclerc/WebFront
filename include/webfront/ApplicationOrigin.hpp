// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-18
//
// WebFront — core
// Role: Canonical application authority (host, port, scheme) shared by the HTTP server and the frontend URL builders.
// Thread: worker-safe | Alloc: heap-init
// LLM: pure string helpers; keep in sync with the origin checks of ContentPolicy.hpp

#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace webfront::detail {

inline void validateApplicationAddress(std::string_view address) {
    if (address.empty())
        throw std::invalid_argument("WebFront: empty frontend address");
    if (address.find_first_of("/?#@ \t\r\n\\\"'") != std::string_view::npos)
        throw std::invalid_argument("WebFront: invalid frontend address");
}

inline std::string applicationHost(std::string_view address) {
    validateApplicationAddress(address);
    if (address == "0.0.0.0")
        return "127.0.0.1";
    if (address == "::")
        return "[::1]";
    if (address.find(':') != std::string_view::npos && address.front() != '[')
        return "[" + std::string(address) + "]";
    return std::string(address);
}

/** @brief Canonical application authority shared by the HTTP server and frontend URL builders. */
inline std::string applicationOrigin(std::string_view address, std::string_view port, bool secure = false) {
    if (port.empty())
        throw std::invalid_argument("WebFront: empty frontend port");
    if (port.find_first_not_of("0123456789") != std::string_view::npos)
        throw std::invalid_argument("WebFront: invalid frontend port");
    return (secure ? "https://" : "http://") + applicationHost(address) + ":" + std::string(port);
}

}  // namespace webfront::detail
