/// @brief Optional type descriptors (ADR-018, #383): the name, label, kind, bounds and accessors of each member of a C++
/// type, written by hand in C++23 or produced by the C++26 reflection layer (Reflection.hpp). Form.hpp builds forms and
/// inspectors from them; typescriptDeclaration() writes the TypeScript declaration that `wf check` verifies.
#pragma once
#include "../../utils/Json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <ios>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::forms {

/// What a member holds, which selects its input and its TypeScript type.
enum class Kind : std::uint8_t { boolean, integer, number, text, choice };

/// Inclusive bounds of a numeric member; a value outside them is refused.
struct Bounds {
    double min{};
    double max{};
};

/** @brief One member of T. read() formats the member as the browser shows it; write() parses a browser value into the
 * member and returns false, leaving it unchanged, when the value does not parse or is out of bounds or choices.
 */
template <typename T>
struct Field {
    std::string                               name;
    std::string                               label;
    Kind                                      kind{};
    std::optional<Bounds>                     range;
    std::vector<std::string>                  choices;
    std::function<std::string(const T&)>      read;
    std::function<bool(T&, std::string_view)> write;
};

/// The descriptor of a type: its TypeScript name and its fields, in display order.
template <typename T>
struct Description {
    std::string           name;
    std::vector<Field<T>> fields;
};

namespace detail {

// Decimal text in the classic locale, so that the result never depends on the application's global locale, nor on the
// standard: C++26 formats std::to_string(double) like std::format (P2587).
template <std::floating_point F>
std::string formatNumber(F value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(std::numeric_limits<F>::digits10);
    stream << value;
    return stream.str();
}

template <typename F>
std::optional<F> parseNumber(std::string_view text) {
    F value{};
    if constexpr (std::floating_point<F>) {
        // std::from_chars for floating point is missing from older libc++; a classic-locale stream is portable.
        if (text.empty() || text.find_first_not_of("+-.0123456789eE") != std::string_view::npos)
            return std::nullopt;
        std::istringstream stream{std::string(text)};
        stream.imbue(std::locale::classic());
        if (!(stream >> value) || stream.peek() != std::char_traits<char>::eof() || !std::isfinite(value))
            return std::nullopt;
    } else {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size())
            return std::nullopt;
    }
    return value;
}

inline bool inside(double value, const std::optional<Bounds>& range) {
    return !range || (value >= range->min && value <= range->max);
}

inline bool identifier(std::string_view name) {
    constexpr std::string_view first = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_$";
    return !name.empty() && first.find(name.front()) != std::string_view::npos && std::ranges::all_of(name, [first](char c) {
        return first.find(c) != std::string_view::npos || (c >= '0' && c <= '9');
    });
}

// A comment cannot end early: "*/" in a label becomes "*\/".
inline std::string comment(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        result += text[i];
        if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/')
            result += '\\';
        else if (text[i] == '\n' || text[i] == '\r')
            result.back() = ' ';
    }
    return result;
}

}  // namespace detail

/** @brief Describes a bool, arithmetic or std::string member. The label defaults to the name; bounds apply only to
 * numbers. Enumerations use choice(), which lists their values.
 */
template <typename T, typename M>
Field<T> field(std::string name, M T::* member, std::string label = {}, std::optional<Bounds> range = {}) {
    Field<T> result{.name = std::move(name), .label = std::move(label), .kind = {}, .range = range, .choices = {}, .read = {}, .write = {}};
    if constexpr (std::same_as<M, bool>) {
        result.kind = Kind::boolean;
        result.read = [member](const T& object) {
            return std::string(object.*member ? "true" : "false");
        };
        result.write = [member](T& object, std::string_view text) {
            if (text != "true" && text != "false")
                return false;
            object.*member = text == "true";
            return true;
        };
    } else if constexpr (std::is_arithmetic_v<M>) {
        result.kind = std::floating_point<M> ? Kind::number : Kind::integer;
        result.read = [member](const T& object) {
            if constexpr (std::floating_point<M>)
                return detail::formatNumber(object.*member);
            else
                return std::to_string(object.*member);
        };
        result.write = [member, range](T& object, std::string_view text) {
            const auto parsed = detail::parseNumber<M>(text);
            if (!parsed || !detail::inside(static_cast<double>(*parsed), range))
                return false;
            object.*member = *parsed;
            return true;
        };
    } else if constexpr (std::same_as<M, std::string>) {
        result.kind = Kind::text;
        result.read = [member](const T& object) {
            return object.*member;
        };
        result.write = [member](T& object, std::string_view text) {
            object.*member = std::string(text);
            return true;
        };
    } else {
        static_assert(sizeof(M) == 0, "Describe enumerations with forms::choice; other member types are not supported");
    }
    if (result.label.empty())
        result.label = result.name;
    return result;
}

/// Describes a member that takes one of the listed values, shown and declared by their names.
template <typename T, typename M>
Field<T> choice(std::string name, M T::* member, std::vector<std::pair<std::string, M>> values, std::string label = {}) {
    static_assert(std::equality_comparable<M>, "A choice member must be equality comparable");
    Field<T> result{.name = std::move(name), .label = std::move(label), .kind = Kind::choice, .range = {}, .choices = {}, .read = {}, .write = {}};
    for (const auto& value : values)
        result.choices.push_back(value.first);
    if (result.label.empty())
        result.label = result.name;
    auto shared = std::make_shared<const std::vector<std::pair<std::string, M>>>(std::move(values));
    result.read = [member, shared](const T& object) {
        for (const auto& [text, value] : *shared)
            if (object.*member == value)
                return text;
        return std::string{};
    };
    result.write = [member, shared](T& object, std::string_view text) {
        for (const auto& [candidate, value] : *shared)
            if (candidate == text) {
                object.*member = value;
                return true;
            }
        return false;
    };
    return result;
}

/** @brief Checks what the generators rely on, and throws std::invalid_argument otherwise: identifier names (they become
 * TypeScript members and input names), unique fields and choices, accessors present, finite ordered bounds on numbers only.
 */
template <typename T>
void validate(const Description<T>& description) {
    if (!detail::identifier(description.name))
        throw std::invalid_argument("Form description name is not an identifier: " + description.name);
    std::set<std::string_view> names;
    for (const auto& field : description.fields) {
        if (!detail::identifier(field.name) || !names.insert(field.name).second)
            throw std::invalid_argument("Form field name is not a unique identifier: " + field.name);
        if (!field.read || !field.write)
            throw std::invalid_argument("Form field " + field.name + " has no accessors");
        if (field.kind > Kind::choice)
            throw std::invalid_argument("Form field " + field.name + " has an unknown kind");
        if (field.range && (field.kind != Kind::integer && field.kind != Kind::number))
            throw std::invalid_argument("Form field " + field.name + " has bounds but is not a number");
        if (field.range && !(std::isfinite(field.range->min) && std::isfinite(field.range->max) && field.range->min <= field.range->max))
            throw std::invalid_argument("Form field " + field.name + " has invalid bounds");
        if ((field.kind == Kind::choice) == field.choices.empty())
            throw std::invalid_argument("Form field " + field.name + " must list choices exactly when it is a choice");
        std::set<std::string_view> choices;
        for (const auto& choice : field.choices)
            if (choice.empty() || !choices.insert(choice).second)
                throw std::invalid_argument("Form field " + field.name + " has an empty or repeated choice");
    }
}

/** @brief The TypeScript interface of the type, for a `.d.ts` file of the project that `wf check` verifies: numbers
 * are `number`, choices a union of their names. Labels and bounds become documentation comments.
 */
template <typename T>
std::string typescriptDeclaration(const Description<T>& description) {
    validate(description);
    std::string result = "export interface " + description.name + " {\n";
    for (const auto& field : description.fields) {
        std::string documentation = field.label;
        if (field.range)
            documentation += " (" + detail::formatNumber(field.range->min) + " to " + detail::formatNumber(field.range->max) + ")";
        if (documentation != field.name)
            result += "    /** " + detail::comment(documentation) + " */\n";
        result += "    readonly " + field.name + ": ";
        switch (field.kind) {
            case Kind::boolean:
                result += "boolean";
                break;
            case Kind::integer:
            case Kind::number:
                result += "number";
                break;
            case Kind::text:
                result += "string";
                break;
            case Kind::choice:
                for (std::size_t i = 0; i < field.choices.size(); ++i)
                    result += (i ? " | " : "") + json::quote(field.choices[i]);
                break;
        }
        result += ";\n";
    }
    return result + "}\n";
}

}  // namespace webfront::forms
