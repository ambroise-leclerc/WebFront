/** @brief HTTP encoding token, parameter and wildcard negotiation for embedded assets. */
#pragma once

#include <algorithm>
#include <string_view>

namespace webfront::http::encoding {

inline std::string_view trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string_view::npos)
        return {};
    return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}

inline std::string_view takePart(std::string_view& remaining, char separator) {
    const auto end  = remaining.find(separator);
    const auto part = trim(remaining.substr(0, end));
    remaining       = end == std::string_view::npos ? std::string_view{} : remaining.substr(end + 1);
    return part;
}

constexpr unsigned char asciiLower(unsigned char value) {
    if (value < 'A' || value > 'Z')
        return value;
    return static_cast<unsigned char>(value + ('a' - 'A'));
}

constexpr bool tokenEquals(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](unsigned char first, unsigned char second) {
        return asciiLower(first) == asciiLower(second);
    });
}

constexpr std::string_view canonicalCoding(std::string_view coding) {
    return tokenEquals(coding, "x-gzip") ? std::string_view("gzip") : coding;
}

constexpr bool isGzip(std::string_view coding) {
    return tokenEquals(canonicalCoding(coding), "gzip");
}

inline bool validQuality(std::string_view value) {
    if (value.empty())
        return false;
    if (value.size() > 5)
        return false;
    if (std::string_view("01").find(value.front()) == std::string_view::npos)
        return false;
    if (value.size() == 1)
        return true;
    if (value[1] != '.')
        return false;
    const std::string_view digits = value[0] == '1' ? "0" : "0123456789";
    return std::all_of(value.begin() + 2, value.end(), [digits](char digit) {
        return digits.find(digit) != std::string_view::npos;
    });
}

inline bool positiveQuality(std::string_view value) {
    if (!validQuality(value))
        return false;
    return value.find_first_of("123456789") != std::string_view::npos;
}

inline bool parametersAccept(std::string_view parameters) {
    while (!parameters.empty()) {
        auto       parameter = takePart(parameters, ';');
        const auto name      = takePart(parameter, '=');
        if (!tokenEquals(name, "q"))
            continue;
        if (!positiveQuality(trim(parameter)))
            return false;
    }
    return true;
}

/** @brief Duplicate mentions accept a coding if any valid mention accepts it, independent of field order.
 * Explicit coding mentions still override the wildcard. Malformed qvalues (including .5) reject that mention.
 */
class Preferences {
public:
    explicit Preferences(std::string_view requestedEncoding) : coding(canonicalCoding(requestedEncoding)) {}

    void add(std::string_view field) {
        while (!field.empty())
            addItem(takePart(field, ','));
    }

    bool accepted() const {
        if (explicitValue != Preference::unlisted)
            return explicitValue == Preference::accepted;
        if (wildcard != Preference::unlisted)
            return wildcard == Preference::accepted;
        return tokenEquals(coding, "identity");
    }

private:
    enum class Preference { unlisted, rejected, accepted };

    static void merge(Preference& preference, bool enabled) {
        if (preference == Preference::accepted)
            return;
        preference = enabled ? Preference::accepted : Preference::rejected;
    }

    void addItem(std::string_view item) {
        const auto token = canonicalCoding(takePart(item, ';'));
        if (tokenEquals(token, coding)) {
            merge(explicitValue, parametersAccept(item));
            return;
        }
        if (token == "*")
            merge(wildcard, parametersAccept(item));
    }

    std::string_view coding;
    Preference       explicitValue = Preference::unlisted;
    Preference       wildcard      = Preference::unlisted;
};

}  // namespace webfront::http::encoding
