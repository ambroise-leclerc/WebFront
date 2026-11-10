/// @brief Byte range requests (RFC 9110 §14): which part of a representation a `Range` header selects.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace webfront::http::range {

enum class Outcome {
    whole,          ///< No usable range: answer 200 with the entire representation.
    partial,        ///< Answer 206 with `[first, first + length)`.
    unsatisfiable   ///< Answer 416.
};

struct Selection {
    Outcome       outcome = Outcome::whole;
    std::uint64_t first   = 0;
    std::uint64_t length  = 0;
};

namespace detail {

inline std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

/// Digits only; a number too large for 64 bits saturates, which every comparison below treats as "past the end".
inline std::optional<std::uint64_t> number(std::string_view digits) {
    if (digits.empty()) return {};
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return {};
        const auto digit = static_cast<std::uint64_t>(c - '0');
        value            = value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10 ? std::numeric_limits<std::uint64_t>::max() : value * 10 + digit;
    }
    return value;
}

inline bool startsWithBytes(std::string_view spec) {
    constexpr std::string_view unit = "bytes=";
    return spec.size() >= unit.size() && std::equal(unit.begin(), unit.end(), spec.begin(), [](char expected, char c) {
               return expected == (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
           });
}

} // namespace detail

/// The part of a representation of `size` bytes that the request asks for.
///
/// Anything the server may ignore is ignored, and the whole representation is answered: a unit other than `bytes`, a
/// malformed or inverted range, several ranges (in one field or several fields), and a range conditioned by an
/// `If-Range` that the representation's validator does not strictly match. Only a single, well-formed range that
/// starts past the end (or an empty suffix) is unsatisfiable.
/// @tparam Fields a type with `getHeadersValues(name)`, such as `http::Request`
/// @param entityTag the strong entity tag of the representation, quoted, when it has one
template <typename Fields>
[[nodiscard]] Selection select(const Fields& request, std::uint64_t size, const std::optional<std::string>& entityTag) {
    const auto ranges = request.getHeadersValues("Range");
    if (ranges.size() != 1) return {};

    const auto conditions = request.getHeadersValues("If-Range");
    if (conditions.size() > 1) return {};
    if (!conditions.empty()) {
        // A date cannot match: the representation has no Last-Modified. A weak tag never matches (strong comparison).
        const auto condition = detail::trim(conditions.front());
        if (!entityTag || condition != *entityTag) return {};
    }

    auto spec = detail::trim(ranges.front());
    if (!detail::startsWithBytes(spec)) return {};
    spec.remove_prefix(6);
    if (spec.find(',') != std::string_view::npos) return {};
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) return {};
    const auto firstText = spec.substr(0, dash);
    const auto lastText  = spec.substr(dash + 1);

    if (firstText.empty()) {  // "-n": the last n bytes
        const auto suffix = detail::number(lastText);
        if (!suffix) return {};
        if (*suffix == 0 || size == 0) return {Outcome::unsatisfiable};
        const auto length = std::min(*suffix, size);
        return {Outcome::partial, size - length, length};
    }
    const auto first = detail::number(firstText);
    if (!first) return {};
    std::optional<std::uint64_t> last;
    if (!lastText.empty()) {
        last = detail::number(lastText);
        if (!last || *last < *first) return {};
    }
    if (*first >= size) return {Outcome::unsatisfiable};
    const auto end = last ? std::min(*last, size - 1) : size - 1;
    return {Outcome::partial, *first, end - *first + 1};
}

/// The `Content-Range` of a 206: `bytes first-last/size`.
[[nodiscard]] inline std::string contentRange(const Selection& selection, std::uint64_t size) {
    return "bytes " + std::to_string(selection.first) + "-" + std::to_string(selection.first + selection.length - 1) + "/" + std::to_string(size);
}

/// The `Content-Range` of a 416: `bytes */size`.
[[nodiscard]] inline std::string unsatisfiedContentRange(std::uint64_t size) {
    return "bytes */" + std::to_string(size);
}

} // namespace webfront::http::range
