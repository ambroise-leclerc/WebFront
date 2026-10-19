#pragma once

#include "../ApplicationOrigin.hpp"
#include "../http/Encodings.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace webfront::frontend::detail {

inline bool hasPercentEncoding(std::string_view page, std::size_t index) {
    if (index + 2 >= page.size())
        return false;
    constexpr std::string_view hex = "0123456789abcdefABCDEF";
    return hex.find(page[index + 1]) != std::string_view::npos && hex.find(page[index + 2]) != std::string_view::npos;
}

inline bool isLiteralPageByte(char byte, bool inPath) {
    // The server decoder maps '+' to a space, so a path plus must be escaped.
    if (byte == '+')
        return !inPath;
    constexpr std::string_view literal = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/?:@!$&()*,;=#";
    return literal.find(byte) != std::string_view::npos;
}

inline std::string encodePageReference(std::string_view page) {
    const auto  pathEnd = page.find_first_of("?#");
    std::string encoded;
    encoded.reserve(page.size());
    for (std::size_t index = 0; index < page.size(); ++index) {
        const auto byte    = static_cast<unsigned char>(page[index]);
        const bool escaped = byte == '%' && hasPercentEncoding(page, index);
        if (escaped || isLiteralPageByte(page[index], index < pathEnd)) {
            encoded += static_cast<char>(byte);
        } else {
            uri::detail::appendPercentEncodedByte(encoded, byte);
        }
    }
    return encoded;
}

// A page is a UTF-8 URI reference: query/fragment delimiters and valid %XX escapes retain their meaning.
inline std::string applicationUrl(std::string_view address, std::string_view port, std::string_view page) {
    return webfront::detail::applicationOrigin(address, port) + "/" + encodePageReference(page);
}

inline std::string applicationUrl(std::string_view port, std::string_view page) {
    return applicationUrl("127.0.0.1", port, page);
}

}  // namespace webfront::frontend::detail
