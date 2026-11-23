// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-19
//
// WebFront — components/pty
// Role: Options and validation of a native pseudoterminal: size, program, environment.
// Thread: worker-safe | Alloc: heap-init
// LLM: optional component; prepare() fills terminal defaults before the platform backend starts

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace webfront {

struct PtySize {
    std::uint16_t columns = 80;
    std::uint16_t rows    = 24;
};

struct PtyOptions {
    std::vector<std::string> argv;
    std::filesystem::path    directory;
    /// Complete child environment, with terminal defaults supplied by Pty.
    std::map<std::string, std::string> environment;
    PtySize                            size;
};

namespace pty::detail {
inline void validateSize(PtySize size) {
    if (!size.columns || !size.rows || size.columns > 32767 || size.rows > 32767)
        throw std::invalid_argument("PTY dimensions must be between 1 and 32767");
}

inline PtyOptions prepare(PtyOptions options) {
    validateSize(options.size);
    if (options.argv.empty() || options.argv.front().empty())
        throw std::invalid_argument("PTY requires an executable");
    for (const auto& arg : options.argv)
        if (arg.contains('\0'))
            throw std::invalid_argument("PTY arguments cannot contain NUL");
    for (const auto& [key, value] : options.environment)
        if (key.empty() || key.contains('=') || key.contains('\0') || value.contains('\0'))
            throw std::invalid_argument("Invalid PTY environment entry");
    options.directory = std::filesystem::absolute(options.directory.empty() ? std::filesystem::current_path() : options.directory);
    if (options.directory.native().contains(std::filesystem::path::value_type{}))
        throw std::invalid_argument("PTY directory cannot contain NUL");
    options.environment.try_emplace("TERM", "xterm-256color");
    options.environment.try_emplace("COLORTERM", "truecolor");
    return options;
}
}  // namespace pty::detail
}  // namespace webfront
