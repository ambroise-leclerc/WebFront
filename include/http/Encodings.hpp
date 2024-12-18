/// @date 16/01/2022 22:27:42
/// @author Ambroise Leclerc
/// @brief Encoders/Decoders for HTTP/WS (URI, BASE64, SHA-1)
#pragma once
#include "../details/C++20Support.hpp"
#include "../details/C++23Support.hpp"
#include "../tooling/Logger.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <span>
#include <string>
#include <string_view>

#include <iostream>

namespace webfront {
namespace uri {

[[nodiscard]] inline std::string encode(std::string_view uri) {
    std::string encoded;
    static std::array digits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    for (auto c : uri) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded += c;
        else {
            encoded += "%";
            encoded += digits[static_cast<size_t>(c >> 4)];
            encoded += digits[static_cast<size_t>(c % 16)];
        }
    }
    return encoded;
}

[[nodiscard]] inline std::string decode(std::string_view uri) {
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t index = 0; index < uri.size(); ++index) {
        switch (uri[index]) {
        case '%':
            if (index + 3 <= uri.size() && std::isxdigit(uri[index + 1]) && std::isxdigit(uri[index + 2])) {
                auto hexToValue = [](char c) -> uint8_t {
                    if (c >= '0' && c <= '9') return uint8_t(c - '0');
                    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
                    return uint8_t(c - 'a' + 10);
                };
                decoded += char(hexToValue(uri[index + 1]) * 16 + hexToValue(uri[index + 2]));
                index += 2;
            }
            else
                decoded += uri[index];
            break;
        case '+': decoded += ' '; break;
        default: decoded += uri[index];
        }
    }
    return decoded;
}

// A representation of an URI with its different fields
//
//          userinfo       host      port
//          ┌──┴───┐ ┌──────┴──────┐ ┌┴┐
//  https://john.doe@www.example.com:123/forum/questions/?tag=networking&order=newest#top
//  └─┬─┘   └─────────────┬────────────┘└───────┬───────┘ └────────────┬────────────┘ └┬┘
//  scheme          authority                  path                  query           fragment
struct URI {
    URI(std::string uri) : string(std::move(uri)), scheme(next(string)), authority(next(string)), path(next(string)), query(next(string)),
        fragment(next(string)), userinfo(next(authority)), host(next(authority)), port(next(authority)) {}
        
    const std::string string;
    const std::string_view scheme, authority, path, query, fragment, userinfo, host, port;

private:    
    std::string_view next(std::string_view uri) const {
        static constexpr size_t schem{0}, autho{1}, path_{2}, quer_{3}, fragm{4}, usrnf{5}, host_{6}, port_{7}, npos = std::string_view::npos; 
        static size_t step{port_}, mark{0}, mark2{0};
        step = (step == port_) ? schem : step + 1;
        switch (step) {
            case schem: mark = uri.find("://"); return mark == npos ? std::string_view{} : uri.substr(0, mark);
            case autho: mark2 = uri.find("/", scheme.empty() ? 0 : mark + 3);
                if (mark2 == npos) { mark2 = uri.size() - 1; return {}; }
                if (scheme.empty()) return {};
                return uri.substr(mark + 3, mark2 - mark - 3);
            case path_: mark = uri.find("?", scheme.empty() ? 0 : mark2 + 1);
                if (mark == npos) {
                    mark = uri.find("#");
                    if (mark == npos) { mark = uri.size() - 1; return uri.substr(mark2); }
                }
                return uri.substr(mark2, mark - mark2);
            case quer_: mark2 = uri.find("#", mark + 1);
                if (mark2 == npos) { mark2 = uri.size() - 1; return uri.substr(mark + 1); } else return uri.substr(mark + 1, mark2 - mark - 1);
            case fragm: return uri.substr(mark2 + 1);
            case usrnf: mark = uri.find("@");
                if (mark == npos) return {}; else return uri.substr(0, mark);
            case host_: mark2 = uri.find(":", mark + 1);
                if (mark2 == npos) return uri.substr(mark + 1); else return uri.substr(mark + 1, mark2 - mark - 1);
            case port_: if (mark2 != npos) return uri.substr(mark2 + 1);
        }
        return {};           
    }
};
} // namespace uri

namespace base64 {

namespace detail {

[[nodiscard]]inline std::string encode(const uint8_t* input, size_t size) {
    static constexpr std::array code = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
                                    'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
                                    's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};

    std::string output((4 * (size + 2) / 3), '\0');
    auto coded = output.begin();
    size_t i = 0;
    for (; i < size - 2; i += 3) {
        *coded++ = code[(input[i] >> 2) & 0x3Fu];
        *coded++ = code[((input[i] & 0x3u) << 4) | ((input[i + 1] & 0xF0u) >> 4)];
        *coded++ = code[((input[i + 1] & 0xFu) << 2) | ((input[i + 2] & 0xC0u) >> 6)];
        *coded++ = code[input[i + 2] & 0x3Fu];
    }
    if (i < size) {
        *coded++ = code[(input[i] >> 2) & 0x3Fu];
        if (i == (size - 1)) {
            *coded++ = code[((input[i] & 0x3u) << 4)];
            *coded++ = '=';
        }
        else {
            *coded++ = code[((input[i] & 0x3u) << 4) | ((input[i + 1] & 0xF0u) >> 4)];
            *coded++ = code[((input[i + 1] & 0xFu) << 2)];
        }
        *coded++ = '=';
    }
    if (coded != output.end()) output.erase(coded);

    return output;
}
} // namespace detail

template<typename T>
concept Container = ::std::movable<T> || requires(T t) {
    t.data();
    t.size();
    T::value_type;
};

[[nodiscard]] inline std::string encode(Container auto input) {
    return detail::encode(reinterpret_cast<const uint8_t*>(input.data()), input.size() * sizeof(typename decltype(input)::value_type));
}

[[nodiscard]] inline std::string encodeInNetworkOrder(Container auto input) {
    if constexpr (std::endian::native == std::endian::little)
        for (auto& elem : input) elem = std::byteswap(elem);

    return encode(std::move(input));
}

} // namespace base64

namespace crypto {
static constexpr size_t sha1Length = 5;
[[nodiscard]] constexpr std::array<uint32_t, sha1Length> sha1(std::string_view input) {
    std::array<uint32_t, sha1Length> digest = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::array<uint32_t, 64> block{0};
    size_t blockByteIndex{0}, byteCount{0};
    auto next = [&](uint8_t byte) {
        block[blockByteIndex++] = byte;
        ++byteCount;
        if ((blockByteIndex = blockByteIndex % 64) == 0) {
            auto leftRotate = [](uint32_t value, size_t count) { return (value << count) ^ (value >> (32 - count)); };
            uint32_t a{digest[0]}, b{digest[1]}, c{digest[2]}, d{digest[3]}, e{digest[4]}, w[80];
            for (size_t i = 0; i < 16; i++) w[i] = (block[i * 4 + 0] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | (block[i * 4 + 3]);
            for (size_t i = 16; i < 80; i++) w[i] = leftRotate((w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]), 1);
            for (std::size_t i = 0; i < 80; ++i) {
                uint32_t f{0}, k{0};
                if (i < 20) {
                    f = (b & c) | (~b & d);
                    k = 0x5A827999;
                }
                else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                }
                else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                }
                else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                auto temp = leftRotate(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = leftRotate(b, 30);
                b = a;
                a = temp;
            }
            digest[0] += a;
            digest[1] += b;
            digest[2] += c;
            digest[3] += d;
            digest[4] += e;
        }
    };

    for (auto c : input) next(static_cast<uint8_t>(c));
    auto b = byteCount * 8;
    next(0x80);
    if (blockByteIndex > 56)
        while (blockByteIndex != 0) next(0);
    while (blockByteIndex < 56) next(0);
    for (auto value : {uint8_t(0), uint8_t(0), uint8_t(0), uint8_t(0), uint8_t(b >> 24), uint8_t(b >> 16), uint8_t(b >> 8), uint8_t(b)}) next(value);

    return digest;
}

[[nodiscard]] inline std::string sha1String(std::string_view input) {
    static const char* digits = "0123456789abcdef";
    std::string output;
    for (auto d : sha1(input))
        for (int8_t shift = 28; shift >= 0; shift -= 4) output += digits[(d >> shift) & 0xF];

    return output;
}

} // namespace crypto
} // namespace webfront