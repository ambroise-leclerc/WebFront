/// @brief Development-time check of CSS font stacks (#611).
///
/// WebKitGTK resolves every family of a stack that is absent, or generic and not a classic keyword, by walking all the
/// installed fonts, for each weight and style the page uses: a stack of four or more such families costs hundreds of
/// milliseconds on the first layout of a machine with a full font set. The check only counts families; it cannot know
/// which are installed, so it is a hint printed by the development file system, never an error.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace webfront::fonts {

/// A `font-family` declaration with at least this many families that are not classic generic keywords is reported.
inline constexpr std::size_t costlyFamilyThreshold = 4;

/// One reported declaration: the stack as written and the number of its families that cost a font walk.
struct LongStack {
    std::string stack;
    std::size_t costly = 0;
};

namespace detail {

inline std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

inline std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

/// The families of a stack, split on commas outside quotes, unquoted and lower-cased.
inline std::vector<std::string> families(std::string_view stack) {
    std::vector<std::string> result;
    std::string              current;
    char                     quote = 0;
    const auto               flush = [&] {
        const auto name = trim(current);
        if (!name.empty())
            result.push_back(lower(name));
        current.clear();
    };
    for (const char c : stack) {
        if (quote) {
            if (c == quote)
                quote = 0;
            else
                current.push_back(c);
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ',') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return result;
}

/// The keywords that the engine maps to one font without a search.
inline bool classicGeneric(const std::string& family) {
    static constexpr std::array<std::string_view, 5> keywords{"serif", "sans-serif", "monospace", "cursive", "fantasy"};
    return std::ranges::find(keywords, family) != keywords.end();
}

}  // namespace detail

namespace detail {

/// The stylesheet without its comments, which are replaced by a space; quoted strings are kept as they are, and a comment
/// opener inside one is text.
inline std::string withoutComments(std::string_view css) {
    std::string clean;
    clean.reserve(css.size());
    char quote = 0;
    for (std::size_t i = 0; i < css.size(); ++i) {
        const char c = css[i];
        if (quote) {
            clean.push_back(c);
            if (c == '\\' && i + 1 < css.size())
                clean.push_back(css[++i]);
            else if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
            clean.push_back(c);
        } else if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const auto end = css.find("*/", i + 2);
            if (end == std::string_view::npos)
                break;
            i = end + 1;
            clean.push_back(' ');
        } else {
            clean.push_back(c);
        }
    }
    return clean;
}

/// Whether `text` has `word` at `at`, ignoring the case (CSS property names are case-insensitive).
inline bool wordAt(std::string_view text, std::size_t at, std::string_view word) {
    if (at + word.size() > text.size())
        return false;
    for (std::size_t i = 0; i < word.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[at + i])) != word[i])
            return false;
    return true;
}

}  // namespace detail

/// The `font-family` declarations of a stylesheet that list too many costly families. Comments and quoted strings (such as
/// the value of `content`) are not declarations; the property name is read whatever its case. A declaration whose value
/// uses `var()` or `inherit` is not analysed. Pure and total: any text is accepted.
inline std::vector<LongStack> longStacks(std::string_view css) {
    const auto             clean = detail::withoutComments(css);
    std::vector<LongStack> found;
    constexpr std::string_view property = "font-family";
    char quote = 0;
    for (std::size_t at = 0; at < clean.size(); ++at) {
        const char c = clean[at];
        if (quote) {
            if (c == '\\')
                ++at;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (!detail::wordAt(clean, at, property))
            continue;
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(clean[at - 1])) || clean[at - 1] == '-' || clean[at - 1] == '_'))
            continue;  // `--font-family` or `x-font-family`: a custom property, not the declaration
        auto colon = at + property.size();
        while (colon < clean.size() && std::isspace(static_cast<unsigned char>(clean[colon])))
            ++colon;
        if (colon >= clean.size() || clean[colon] != ':')
            continue;
        // The value ends at the first `;` or `}` outside quotes.
        std::size_t end = colon + 1;
        char        valueQuote = 0;
        for (; end < clean.size(); ++end) {
            const char v = clean[end];
            if (valueQuote) {
                if (v == '\\')
                    ++end;
                else if (v == valueQuote)
                    valueQuote = 0;
            } else if (v == '"' || v == '\'') {
                valueQuote = v;
            } else if (v == ';' || v == '}') {
                break;
            }
        }
        end = std::min(end, clean.size());
        at  = end;  // resume after the declaration (the loop increments past the terminator)
        const auto value = std::string(detail::trim(std::string_view(clean).substr(colon + 1, end - colon - 1)));
        if (value.find("var(") != std::string::npos || detail::lower(value) == "inherit")
            continue;
        std::size_t costly = 0;
        for (const auto& family : detail::families(value))
            if (!detail::classicGeneric(family))
                ++costly;
        if (costly >= costlyFamilyThreshold)
            found.push_back({value, costly});
    }
    return found;
}

/// Where the warnings go: std::cerr when null. Tests point it at a stream of their own.
inline std::ostream* warningOutput = nullptr;

/// The version of a stylesheet file: its size and its last write time, compared as they are (no conversion of the file
/// clock's representation, which differs between standard libraries).
struct Stamp {
    std::uintmax_t                size = 0;
    std::filesystem::file_time_type written{};
    friend bool operator==(const Stamp&, const Stamp&) = default;
};

namespace detail {
inline std::mutex& seenMutex() {
    static std::mutex mutex;
    return mutex;
}
/// The version (a stamp such as modification time and size) of each stylesheet last checked, by name.
inline std::map<std::string, Stamp>& seenVersions() {
    static std::map<std::string, Stamp> seen;
    return seen;
}
}  // namespace detail

/// Whether the stylesheet `name` has not been checked yet at this `stamp`, a stamp that changes when the file does
/// (lets a caller skip reading it again, and read it again once it is edited).
inline bool unchecked(const std::string& name, const Stamp& stamp) {
    std::scoped_lock lock{detail::seenMutex()};
    const auto       found = detail::seenVersions().find(name);
    return found == detail::seenVersions().end() || found->second != stamp;
}

/// Writes one warning for each long stack of the stylesheet `name` at `stamp`, once per name and stamp for the life
/// of the process: an edited file (another stamp) is checked again, an unchanged one is not.
/// @return the number of warnings written
inline std::size_t warnLongStacks(const std::string& name, std::string_view css, const Stamp& stamp = {}) {
    {
        std::scoped_lock lock{detail::seenMutex()};
        auto [entry, inserted] = detail::seenVersions().try_emplace(name, stamp);
        if (!inserted) {
            if (entry->second == stamp)
                return 0;
            entry->second = stamp;
        }
    }
    const auto stacks = longStacks(css);
    std::scoped_lock lock{detail::seenMutex()};
    auto&            out = warningOutput ? *warningOutput : std::cerr;
    for (const auto& stack : stacks)
        out << "WebFront: " << name << ": font-family \"" << stack.stack << "\" lists " << stack.costly
            << " families that WebKitGTK looks up in every installed font on the first layout (hundreds of milliseconds; "
               "other engines are not affected this way); put an installed family first, or use sans-serif / monospace (docs/WEBKITGTK.md).\n";
    return stacks.size();
}

}  // namespace webfront::fonts
