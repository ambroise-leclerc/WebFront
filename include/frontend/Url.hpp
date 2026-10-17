#pragma once

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

// A page is a UTF-8 URI reference: query/fragment delimiters and valid %XX escapes retain their meaning.
inline std::string applicationUrl(std::string_view address, std::string_view port, std::string_view page) {
    if (port.empty())
        throw std::invalid_argument("WebFront: empty frontend port");
    if (port.find_first_not_of("0123456789") != std::string_view::npos)
        throw std::invalid_argument("WebFront: invalid frontend port");
    return "http://" + applicationHost(address) + ":" + std::string(port) + "/" + encodePageReference(page);
}

inline std::string applicationUrl(std::string_view port, std::string_view page) {
    return applicationUrl("127.0.0.1", port, page);
}

}  // namespace webfront::frontend::detail
