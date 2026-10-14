/// @brief Minimal JSON support for the small files WebFront writes itself (such as the wf tool
/// manager's lock file): a strict reader of objects and strings, and string quoting.
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace webfront::json {

namespace detail {

inline constexpr std::string_view whitespace      = " \t\n\r";
inline constexpr std::string_view tokenCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.";
inline constexpr std::string_view escapeCodes     = "\"\\/bfnrt";
inline constexpr std::string_view escapedValues   = "\"\\/\b\f\n\r\t";

inline bool isHighSurrogate(std::uint32_t unit) {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

inline bool isLowSurrogate(std::uint32_t unit) {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

inline void appendUtf8(std::string& value, std::uint32_t code) {
    if (code < 0x80) {
        value += static_cast<char>(code);
        return;
    }
    constexpr std::array<std::uint32_t, 4> leadBits{0, 0xC0, 0xE0, 0xF0};
    const int                              continuations = code < 0x800 ? 1 : (code < 0x10000 ? 2 : 3);
    value                                               += static_cast<char>(leadBits[static_cast<std::size_t>(continuations)] | (code >> (6 * continuations)));
    for (int shift = 6 * (continuations - 1); shift >= 0; shift -= 6)
        value += static_cast<char>(0x80 | ((code >> shift) & 0x3F));
}

inline void appendQuoted(std::string& quoted, char c) {
    constexpr std::string_view hexDigits = "0123456789abcdef";
    const auto                 code      = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\')
        (quoted += '\\') += c;
    else if (code < 0x20)
        ((quoted += "\\u00") += hexDigits[code >> 4]) += hexDigits[code & 0xF];
    else
        quoted += c;
}

}  // namespace detail

/// Quotes a string for JSON, escaping quotes, backslashes and control characters.
inline std::string quote(std::string_view text) {
    std::string quoted = "\"";
    for (const char c : text)
        detail::appendQuoted(quoted, c);
    return quoted += '"';
}

/// Strict JSON reader driven by the caller: objects and strings are read, any other value can be
/// skipped. Errors throw std::runtime_error with the offset of the problem.
class Reader {
public:
    explicit Reader(std::string_view json) : text(json) {}

    /// Reads an object, calling onMember(key) for each member; onMember must consume the value.
    void object(const std::function<void(const std::string&)>& onMember) {
        expect('{');
        if (consumeIf('}'))
            return;
        do {
            const auto key = string();
            expect(':');
            onMember(key);
        } while (consumeIf(','));
        expect('}');
    }

    std::string string() {
        expect('"');
        std::string value;
        for (char c = next(); c != '"'; c = next())
            appendCharacter(value, c);
        return value;
    }

    /// Skips any value: object, array, string, number, true, false or null.
    void skipValue() {
        switch (peek()) {
            case '{':
                return object([this](const std::string&) {
                    skipValue();
                });
            case '[':
                return skipArray();
            case '"':
                string();
                return;
            default:
                return skipToken();
        }
    }

    /// Checks that only whitespace follows the value read.
    void end() {
        skipWhitespace();
        if (index != text.size())
            fail("unexpected data after the value");
    }

private:
    std::string_view text;
    std::size_t      index = 0;

    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason + " at offset " + std::to_string(index));
    }

    void skipWhitespace() {
        index = std::min(text.find_first_not_of(detail::whitespace, index), text.size());
    }

    char peek() {
        skipWhitespace();
        return index < text.size() ? text[index] : '\0';
    }

    char next() {
        if (index == text.size())
            fail("unterminated string");
        return text[index++];
    }

    bool consumeIf(char c) {
        if (peek() != c)
            return false;
        ++index;
        return true;
    }

    void expect(char c) {
        if (!consumeIf(c))
            fail(std::string("'") + c + "' expected");
    }

    void skipArray() {
        expect('[');
        if (consumeIf(']'))
            return;
        do
            skipValue();
        while (consumeIf(','));
        expect(']');
    }

    void skipToken() {
        const auto length = std::min(text.find_first_not_of(detail::tokenCharacters, index), text.size()) - index;
        if (length == 0)
            fail("value expected");
        index += length;
    }

    void appendCharacter(std::string& value, char c) {
        if (static_cast<unsigned char>(c) < 0x20)
            fail("control character in a string");
        if (c == '\\')
            escaped(value);
        else
            value += c;
    }

    void escaped(std::string& value) {
        const char c = next();
        if (c == 'u')
            return detail::appendUtf8(value, codePoint());
        const auto found = detail::escapeCodes.find(c);
        if (found == std::string_view::npos)
            fail("invalid escape");
        value += detail::escapedValues[found];
    }

    std::uint32_t hex4() {
        const auto    digits    = text.substr(index, 4);
        std::uint32_t unit      = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), unit, 16);
        if (error != std::errc{} || end != digits.data() + 4)
            fail("invalid \\u escape");
        index += 4;
        return unit;
    }

    std::uint32_t codePoint() {
        const auto unit = hex4();
        if (detail::isLowSurrogate(unit))
            fail("unpaired surrogate");
        if (!detail::isHighSurrogate(unit))
            return unit;
        return 0x10000 + ((unit - 0xD800) << 10) + (lowSurrogate() - 0xDC00);
    }

    std::uint32_t lowSurrogate() {
        if (next() != '\\' || next() != 'u')
            fail("unpaired surrogate");
        const auto unit = hex4();
        if (!detail::isLowSurrogate(unit))
            fail("unpaired surrogate");
        return unit;
    }
};

}  // namespace webfront::json
