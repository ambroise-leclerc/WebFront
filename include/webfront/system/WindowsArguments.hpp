// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-19
//
// WebFront — system
// Role: Windows command line quoting following CommandLineToArgvW and MSVCRT rules.
// Thread: worker-safe | Alloc: heap-init
// LLM: exact quoting matters; the program must receive exactly its argument

#pragma once

#ifdef _WIN32
    #include "WindowsCompat.hpp"

    #include <stdexcept>
    #include <string>

namespace webfront::system::detail {
inline std::wstring toWide(const std::string& text) {
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        throw std::invalid_argument("Text is not valid UTF-8");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

/// The CommandLineToArgvW / MSVCRT quoting: the program receives exactly the argument.
inline std::wstring quoteWindowsArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
            quoted.append(backslashes * 2 + 1, L'\\');
        else
            quoted.append(backslashes, L'\\');
        quoted.push_back(*it);
    }
    return quoted + L'"';
}

}  // namespace webfront::system::detail
#endif
