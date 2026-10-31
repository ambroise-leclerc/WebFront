/// @brief Bounded JSON input handling for the optional local automation endpoint.
#pragma once

#include "../utils/Json.hpp"

#include <charconv>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace webfront::automation {
inline constexpr std::size_t messageLimit = 60000;

// Bound recursion before entering the shared recursive JSON reader. Also reject non-UTF-8 input.
inline void validateInput(std::string_view text, std::size_t limit = messageLimit) {
    if (text.size() > limit)
        throw std::invalid_argument("Automation message exceeds " + std::to_string(limit) + " bytes");
    unsigned depth  = 0;
    bool     quoted = false, escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80) {
            unsigned      count = 0;
            std::uint32_t value = 0;
            if (c >= 0xC2 && c <= 0xDF) {
                count = 1;
                value = c & 0x1F;
            } else if (c >= 0xE0 && c <= 0xEF) {
                count = 2;
                value = c & 0x0F;
            } else if (c >= 0xF0 && c <= 0xF4) {
                count = 3;
                value = c & 7;
            } else
                throw std::invalid_argument("Invalid UTF-8");
            const auto least = count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u;
            for (unsigned n = 0; n < count; ++n) {
                if (++i >= text.size())
                    throw std::invalid_argument("Incomplete UTF-8");
                const auto next = static_cast<unsigned char>(text[i]);
                if ((next & 0xC0) != 0x80)
                    throw std::invalid_argument("Invalid UTF-8 continuation");
                value = (value << 6) | (next & 0x3F);
            }
            if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
                throw std::invalid_argument("Invalid UTF-8 codepoint");
            escaped = false;
            continue;
        }
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32)
                throw std::invalid_argument("Automation JSON nesting exceeds 32");
        } else if (c == '}' || c == ']') {
            if (!depth)
                throw std::invalid_argument("Unbalanced JSON");
            --depth;
        }
    }
}

inline std::string stringValue(std::string_view text) {
    json::Reader reader{text};
    auto         value = reader.string();
    reader.end();
    return value;
}

class Object {
public:
    explicit Object(std::string_view text = "{}", std::size_t limit = messageLimit) {
        validateInput(text, limit);
        json::Reader reader{text};
        reader.object([&](const std::string& key) {
            const auto value = std::string(reader.rawValue());
            if (!fields.emplace(key, value).second)
                throw std::invalid_argument("Duplicate JSON member: " + key);
        });
        reader.end();
    }
    bool has(std::string_view key) const {
        return fields.contains(std::string(key));
    }
    std::string raw(std::string_view key, std::string fallback = "null") const {
        const auto found = fields.find(std::string(key));
        return found == fields.end() ? fallback : found->second;
    }
    std::string string(std::string_view key, std::string fallback = {}) const {
        return has(key) ? stringValue(raw(key)) : fallback;
    }
    void only(std::initializer_list<std::string_view> allowed) const {
        for (const auto& [key, value] : fields)
            if (std::ranges::find(allowed, key) == allowed.end())
                throw std::invalid_argument("Unexpected member: " + key);
    }
    const std::map<std::string, std::string>& members() const {
        return fields;
    }

private:
    std::map<std::string, std::string> fields;
};

inline std::string objectJson(const std::map<std::string, std::string>& members) {
    std::string result = "{";
    for (const auto& [key, value] : members)
        result += (result.size() > 1 ? "," : "") + json::quote(key) + ":" + value;
    return result + "}";
}
inline std::string stringArray(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values)
        result += (result.size() > 1 ? "," : "") + json::quote(value);
    return result + "]";
}
}  // namespace webfront::automation
