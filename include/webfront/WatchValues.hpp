// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — core
// Role: How the inspectors read a C++ object as a tree of nodes (ADR-027, sections 3, 4 and 6): bounds, texts, paths, kinds, rendering.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public API (Visit, PagedSource, Described, the bounds); the per-type tables are internal, read by WatchMemory.hpp

#pragma once

#include "BridgeError.hpp"
#include "components/forms/Description.hpp"
#include "http/BuffersPolicy.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <source_location>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#if __has_include(<cxxabi.h>)
    #include <cstdlib>

    #include <cxxabi.h>
#endif

namespace webfront::inspect {

// --- Bounds (ADR-027, section 3) -------------------------------------------------------------------------------

/// A watch's name, the label and the value of a node, and the texts of a descriptor are at most this many UTF-8 bytes.
inline constexpr std::size_t maxTextBytes = 256;
/// The kind of a node is a word of the closed vocabulary below, at most this many bytes.
inline constexpr std::size_t maxKindBytes = 16;
/// A page holds 200 entries unless the inspector asks for another count, and never more than 1 000.
inline constexpr std::size_t defaultPageCount = 200;
inline constexpr std::size_t maxPageCount     = 1000;
/// The bytes of an answer that are not its entries: the message header, the call identifier and the page's own fields.
inline constexpr std::size_t responseHeaderBytes = 64;
/// The smallest answer the inspectors must be able to send: a page of one node, with its header.
inline constexpr std::size_t minPageBytes = 4096;
/// A path is at most this many bytes and segments; an encoded key at most maxKeyBytes, else its element is told by its position.
inline constexpr std::size_t maxPathBytes    = 1024;
inline constexpr std::size_t maxPathSegments = 64;
inline constexpr std::size_t maxKeyBytes     = 128;
inline constexpr std::size_t maxIndexSegment = 21;               // a letter and the 20 digits of 2^64 - 1
inline constexpr std::size_t maxKeySegment   = 1 + maxKeyBytes;  // `k` and the encoded key
inline constexpr std::size_t maxNodeBytes    = 2048;
/// The nodes a connection keeps of one watch, and the bytes of the snapshot of a full value.
inline constexpr std::size_t maxMemoryNodes   = 10'000;
inline constexpr std::size_t maxSnapshotBytes = 16 * 1024 * 1024;
/// The smallest text an inspector must be able to write, whatever the policy.
inline constexpr std::size_t minWriteTextBytes = 1024;
/// What a write request takes besides its text, all its other arguments at their largest: the message header, the name
/// `webfront.inspector.write`, two numbers, the path and the expected value.
inline constexpr std::size_t writeRequestFixedBytes = 8 + (2 + 24) + 9 + 9 + (3 + maxPathBytes) + (3 + maxTextBytes);

namespace detail {
/// The longest string that, with its header of 2, 3 or 5 bytes, takes at most `room` bytes on the wire.
[[nodiscard]] constexpr std::size_t longestStringIn(std::size_t room) noexcept {
    if (room > 5 + 65535)
        return room - 5;
    if (room > 3 + 255)
        return (std::min<std::size_t>)(room - 3, 65535);
    return room > 2 ? (std::min<std::size_t>)(room - 2, 255) : 0;
}
}  // namespace detail

/// What an answer may take with the policy: it must fit both the encoding buffer and the message size.
template <typename Policy>
inline constexpr std::size_t responseBudget = std::min<std::size_t>(Policy::functionCallBufferSize, http::maxMessageSize<Policy>());
/// What a request may take: C++ closes a connection that announces a larger message (ADR-003).
template <typename Policy>
inline constexpr std::size_t requestBudget = http::maxMessageSize<Policy>();
/// The longest text webfront.inspector.write accepts with the policy: what the request budget leaves once every other
/// argument is at its largest.
template <typename Policy>
inline constexpr std::size_t maxWriteTextBytes = detail::longestStringIn(
    requestBudget<Policy> > writeRequestFixedBytes ? requestBudget<Policy> - writeRequestFixedBytes : 0);

/// The bounds an operation of the inspectors runs with; limitsOf<Policy>() gives those of a buffers policy.
struct Limits {
    std::size_t responseBudget;
    std::size_t maxWriteTextBytes;
    std::size_t maxNodes         = maxMemoryNodes;
    std::size_t maxSnapshotBytes = inspect::maxSnapshotBytes;
};

/**
 * The bounds of a buffers policy. A policy too small for the inspectors does not compile with the developer environment,
 * rather than failing while inspecting: an answer must hold a page of one node, and a write a text of 1 KiB.
 */
template <typename Policy>
[[nodiscard]] constexpr Limits limitsOf() noexcept {
    static_assert(responseBudget<Policy> >= minPageBytes,
                  "The buffers policy is too small for the inspectors: an answer must hold a page of one node (ADR-027)");
    static_assert(maxWriteTextBytes<Policy> >= minWriteTextBytes,
                  "The buffers policy is too small for the inspectors: a write must carry a text of 1 KiB (ADR-027)");
    return {.responseBudget = responseBudget<Policy>, .maxWriteTextBytes = maxWriteTextBytes<Policy>};
}

/// The error codes of the inspectors (typed codes of ADR-003).
namespace code {
inline constexpr std::string_view denied      = "webfront.inspector.denied";       // the caller is not admitted
inline constexpr std::string_view unknown     = "webfront.inspector.unknown";      // no such watch, path or node
inline constexpr std::string_view unavailable = "webfront.inspector.unavailable";  // the source did not give its object
inline constexpr std::string_view limit       = "webfront.inspector.limit";        // depth reached, or the memory is full
inline constexpr std::string_view stale       = "webfront.inspector.stale";        // what the inspector shows is no longer true
inline constexpr std::string_view parse       = "webfront.inspector.parse";        // the text is not a value of the node
inline constexpr std::string_view readOnly    = "webfront.inspector.readOnly";     // the node is not writable
inline constexpr std::string_view tooLarge    = "webfront.inspector.tooLarge";     // the text is longer than maxWriteTextBytes
inline constexpr std::string_view refused     = "webfront.inspector.refused";      // the application refused the value
}  // namespace code

/// The bits of a node's flags.
namespace flag {
inline constexpr unsigned writable        = 1;
inline constexpr unsigned truncated       = 2;
inline constexpr unsigned removed         = 4;
inline constexpr unsigned depthReached    = 8;
inline constexpr unsigned childrenChanged = 16;
}  // namespace flag

/// The prefix of the names reserved to the inspectors: cppFunction refuses them to the application (ADR-027, section 1).
inline constexpr std::string_view reservedPrefix = "webfront.inspector.";

[[nodiscard]] inline bool isReservedName(std::string_view name) noexcept {
    return name.starts_with(reservedPrefix);
}

/// A node as the bridge carries it: (path, kind, label, value, children, flags).
using Node = std::tuple<std::string, std::string, std::string, std::string, double, double>;
/// A page of nodes: (nodes, next, revision, resynchronize). `next` is 0 at the end.
using Page = std::tuple<std::vector<Node>, double, double, bool>;
/// What webfront.inspector.write answers: (revision, code, message); the code is empty when the value was written.
using WriteAnswer = std::tuple<double, std::string, std::string>;
/// What webfront.inspector.value answers: (fragment, total size, next offset, capped, snapshot).
using ValueAnswer = std::tuple<std::string, double, double, bool, double>;
/// What webfront.inspector.limits answers: (maxWriteTextBytes, responseBudget, nodes of the memory, bytes of a snapshot,
/// largest page count, default page count).
using LimitsAnswer = std::tuple<double, double, double, double, double, double>;

/// The largest number a page exchanges exactly with JavaScript (2^53).
inline constexpr double maxExactNumber = 9007199254740992.0;

// --- Texts -----------------------------------------------------------------------------------------------------

/// The longest prefix of `text` of at most `limit` bytes that does not cut a UTF-8 character.
[[nodiscard]] inline std::string_view utf8Prefix(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit)
        return text;
    std::size_t end = limit;
    // Back to the first byte of the character that the limit cuts: continuation bytes are 10xxxxxx.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u)
        --end;
    return text.substr(0, end);
}

namespace detail {
/// Appends `c` to `visible`, a control character as its visible picture (U+2400 to U+241F, U+2421 for DEL).
inline void appendVisible(std::string& visible, char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20u || byte == 0x7Fu) {
        const unsigned picture = byte == 0x7Fu ? 0x2421u : 0x2400u + byte;  // three UTF-8 bytes: E2 90 xx
        visible               += static_cast<char>(0xE2);
        visible               += static_cast<char>(0x80u | ((picture >> 6) & 0x3Fu));
        visible               += static_cast<char>(0x80u | (picture & 0x3Fu));
    } else
        visible += c;
}
inline constexpr std::string_view ellipsis = "\xE2\x80\xA6";

/// `text` with its control characters visible, cut to `limit` bytes on a character boundary (ending with "…" when
/// `mark`), and whether it was cut. Only the bytes needed to know it are read.
[[nodiscard]] inline std::pair<std::string, bool> visibleText(std::string_view text, std::size_t limit, bool mark) {
    std::string visible;
    visible.reserve((std::min)(text.size(), limit));  // parenthesized: Windows headers may define min
    for (const char c : text) {
        appendVisible(visible, c);
        if (visible.size() > limit)
            break;  // the first byte past the limit is enough to know that it is cut, whatever the limit (npos included)
    }
    if (visible.size() <= limit)
        return {std::move(visible), false};
    if (!mark || limit < ellipsis.size())
        return {std::string{utf8Prefix(visible, limit)}, true};
    std::string cut{utf8Prefix(visible, limit - ellipsis.size())};
    cut += ellipsis;
    return {std::move(cut), true};
}
}  // namespace detail

/// A text of the application made safe to show and bounded: control characters become their visible pictures (U+2400
/// to U+241F, U+2421 for DEL), then the text is cut to `limit` bytes on a character boundary, ending with "…" when cut.
/// A limit too small for the ellipsis gives the longest prefix that fits, without it.
[[nodiscard]] inline std::string boundedText(std::string_view text, std::size_t limit = maxTextBytes) {
    return detail::visibleText(text, limit, true).first;
}

/// Where a watch was declared, "file:line", made visible as boundedText() does, then cut from its start when it is too long:
/// the end of the path, the file's name and the line, are what identify it.
[[nodiscard]] inline std::string locationText(std::string_view file, std::uint_least32_t line) {
    std::string visible;
    for (const char c : std::string(file) + ':' + std::to_string(line))
        detail::appendVisible(visible, c);
    if (visible.size() <= maxTextBytes)
        return visible;
    std::size_t start = visible.size() - (maxTextBytes - detail::ellipsis.size());
    while (start < visible.size() && (static_cast<unsigned char>(visible[start]) & 0xC0u) == 0x80u)
        ++start;  // a character is never cut: its first byte is not a continuation byte
    return std::string(detail::ellipsis) + visible.substr(start);
}
[[nodiscard]] inline std::string locationText(const std::source_location& location) {
    return locationText(location.file_name(), location.line());
}

/**
 * @brief A bounded receiver (ADR-027, "Formatage borné"): what is written to it is stored up to its limit only, and it
 * knows whether more was written. A string is copied up to the limit; a formattable value goes through
 * std::format_to_n, which counts the size the whole output would have without storing it.
 */
class TextSink {
public:
    explicit TextSink(std::size_t bound) : limit(bound) { }

    void append(std::string_view part) {
        const auto room = limit - stored.size();
        if (part.size() > room) {
            stored.append(part.substr(0, room));
            cut = true;
        } else
            stored.append(part);
    }
    void append(char c) {
        if (stored.size() < limit)
            stored += c;
        else
            cut = true;
    }
    template <typename T>
    void format(const T& value) {
        const auto room   = limit - stored.size();
        const auto result = std::format_to_n(std::back_inserter(stored), static_cast<std::ptrdiff_t>(room), "{}", value);
        if (static_cast<std::size_t>(result.size) > room)
            cut = true;
    }
    [[nodiscard]] bool full() const noexcept {
        return stored.size() >= limit;
    }
    [[nodiscard]] bool truncated() const noexcept {
        return cut;
    }
    [[nodiscard]] const std::string& text() const noexcept {
        return stored;
    }

private:
    std::string stored;
    std::size_t limit;
    bool        cut = false;
};

// --- Paths (ADR-027, section 3, "Encodage des chemins") --------------------------------------------------------

/// One segment of a path: `#` and an index or a rank, `.` and the rank of a described field, or `k` and a key.
struct Segment {
    char          letter = '#';
    std::uint64_t number = 0;
    std::string   key;

    friend bool operator==(const Segment&, const Segment&) = default;
};

namespace detail {
[[nodiscard]] constexpr bool unreservedByte(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}
[[nodiscard]] constexpr int hexDigit(char c) noexcept {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
}  // namespace detail

/// A key in percent encoding: every byte outside `A-Z a-z 0-9 - . _ ~` becomes `%HH`. Reversible, and never holds a `/`.
[[nodiscard]] inline std::string encodeKey(std::string_view key) {
    static constexpr std::string_view digits = "0123456789ABCDEF";
    std::string                       encoded;
    encoded.reserve(key.size());
    for (const char c : key) {
        if (detail::unreservedByte(c))
            encoded += c;
        else {
            const auto byte = static_cast<unsigned char>(c);
            encoded        += '%';
            encoded        += digits[byte >> 4];
            encoded        += digits[byte & 0x0Fu];
        }
    }
    return encoded;
}

/// The segment of an index, a rank or a field: its letter and the decimal number.
[[nodiscard]] inline std::string numberSegment(char letter, std::uint64_t number) {
    return letter + std::to_string(number);
}

/// The path of a child: its parent's path, `/`, and the segment.
[[nodiscard]] inline std::string childPath(std::string_view parent, std::string_view segment) {
    std::string path;
    path.reserve(parent.size() + 1 + segment.size());
    path += parent;
    path += '/';
    path += segment;
    return path;
}

/// The path of the parent of a node that is not the root.
[[nodiscard]] inline std::string_view parentPath(std::string_view path) noexcept {
    const auto cut = path.rfind('/');
    return cut == std::string_view::npos ? std::string_view{} : path.substr(0, cut);
}

/**
 * The segments of a path, or nothing for a path that is not one this C++ builds: more than 1 024 bytes or 64 segments, a
 * segment without its letter, a number that is not the decimal text of a 64-bit number, a key longer than 128 bytes or
 * that is not exactly what encodeKey() writes. The root is the empty path.
 */
[[nodiscard]] inline std::optional<std::vector<Segment>> parsePath(std::string_view path) {
    std::vector<Segment> segments;
    if (path.empty())
        return segments;
    if (path.size() > maxPathBytes || path.front() != '/')
        return std::nullopt;
    std::size_t start = 1;
    while (true) {
        const auto end  = (std::min)(path.find('/', start), path.size());
        const auto text = path.substr(start, end - start);
        if (text.empty() || segments.size() == maxPathSegments)
            return std::nullopt;
        Segment    segment{.letter = text.front(), .number = 0, .key = {}};
        const auto rest = text.substr(1);
        if (segment.letter == '#' || segment.letter == '.') {
            const auto [parsed, error] = std::from_chars(rest.data(), rest.data() + rest.size(), segment.number);
            // The one text of the number: no sign, no leading zero, nothing after the digits.
            if (error != std::errc{} || parsed != rest.data() + rest.size() || std::to_string(segment.number) != rest)
                return std::nullopt;
        } else if (segment.letter == 'k') {
            if (rest.size() > maxKeyBytes)
                return std::nullopt;
            for (std::size_t i = 0; i < rest.size(); ++i) {
                if (detail::unreservedByte(rest[i]))
                    segment.key += rest[i];
                else if (rest[i] == '%' && i + 2 < rest.size() && detail::hexDigit(rest[i + 1]) >= 0 && detail::hexDigit(rest[i + 2]) >= 0) {
                    const char byte = static_cast<char>(detail::hexDigit(rest[i + 1]) * 16 + detail::hexDigit(rest[i + 2]));
                    if (detail::unreservedByte(byte))
                        return std::nullopt;  // encodeKey() never encodes it: two texts would name one key
                    segment.key += byte;
                    i           += 2;
                } else
                    return std::nullopt;
            }
        } else
            return std::nullopt;
        segments.push_back(std::move(segment));
        if (end == path.size())
            return segments;
        start = end + 1;
    }
}

// --- Sources and descriptions ----------------------------------------------------------------------------------

/**
 * @brief Gives the inspectors the description of T (docs/FORMS.md): specialize it with a static `description()` that
 * returns a `forms::Description<T>`, and a T is shown as its fields (kind `described`), each writable within its bounds.
 *
 *     template <> struct webfront::inspect::Described<Settings> {
 *         static webfront::forms::Description<Settings> description() { return {.name = "Settings", .fields = {...}}; }
 *     };
 *
 * With C++26 reflection (components/forms/Reflection.hpp), `description()` returns `forms::describe<T>()`.
 */
template <typename T>
struct Described;

template <typename T>
concept HasDescription = requires {
    { Described<std::remove_cv_t<T>>::description() } -> std::same_as<forms::Description<std::remove_cv_t<T>>>;
};

namespace detail {
/// What a PagedSource's visit() is checked with: it must accept any element.
struct AnyElement {
    template <typename Element>
    void operator()(Element&&) const noexcept { }
};
}  // namespace detail

/**
 * @brief A container that pages itself (ADR-027, section 5): `size()` and `visit(first, count, function)`, which calls
 * `function(element)` for the elements at `first` to `first + count - 1`, in order. A page then costs what the application
 * decides: it is the way of a very large container without random access, or of a content computed on demand.
 */
template <typename T>
concept PagedSource = !std::ranges::range<T> && requires(T& source, std::size_t position) {
    { source.size() } -> std::convertible_to<std::size_t>;
    source.visit(position, position, detail::AnyElement{});
};

/**
 * @brief A PagedSource whose elements have keys: `key(position)` gives the key of an element and `find(key)` its position
 * now, or nothing. The contract is a primary key's: a key names at most one element, and an element keeps its key while
 * it exists. The path of an element then carries its key, so that an inspector follows it when the container changes.
 */
template <typename T>
concept KeyedPagedSource = PagedSource<T> && requires(T& source, std::size_t position, std::string_view key) {
    { source.key(position) } -> std::convertible_to<std::string>;
    { source.find(key) } -> std::convertible_to<std::optional<std::size_t>>;
};

// --- Kinds (ADR-027, section 4) --------------------------------------------------------------------------------

namespace detail {
template <typename T>
struct is_bitset : std::false_type { };
template <std::size_t N>
struct is_bitset<std::bitset<N>> : std::true_type { };
template <typename T>
struct is_optional : std::false_type { };
template <typename T>
struct is_optional<std::optional<T>> : std::true_type { };
template <typename T>
struct is_variant : std::false_type { };
template <typename... T>
struct is_variant<std::variant<T...>> : std::true_type { };
template <typename T>
struct is_tuple : std::false_type { };
template <typename... T>
struct is_tuple<std::tuple<T...>> : std::true_type { };
template <typename A, typename B>
struct is_tuple<std::pair<A, B>> : std::true_type { };
template <typename T>
concept associative = std::ranges::sized_range<T> && requires {
    typename T::key_type;
    typename T::mapped_type;
};
template <typename T>
concept text = std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;
/// A type std::format can write. std::formattable alone misses the formatters written for std::format_context only, which
/// std::format accepts: the context it checks with is the library's own choice.
template <typename T>
concept formattable = std::formattable<T, char>
                      || (std::default_initializable<std::formatter<T>>
                          && requires(const std::formatter<T>& formatter, const T& value, std::format_context& context) {
                                 { formatter.format(value, context) } -> std::same_as<std::format_context::iterator>;
                             });
}  // namespace detail

namespace detail {
/// What a type is for the inspectors, in the order of ADR-027 section 4. T keeps its constness: a container that only a
/// non-constant object can walk is not one when it is constant.
enum class Shape : std::uint8_t { boolean, integer, real, string, bitset, optional, variant, tuple, map, sequence, paged, described, formatted, opaque };

template <typename T>
[[nodiscard]] constexpr Shape shapeOf() noexcept {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>)
        return Shape::boolean;
    else if constexpr (std::is_integral_v<U>)
        return Shape::integer;
    else if constexpr (std::is_floating_point_v<U>)
        return Shape::real;
    else if constexpr (text<U>)
        return Shape::string;
    else if constexpr (is_bitset<U>::value)
        return Shape::bitset;
    else if constexpr (is_optional<U>::value)
        return Shape::optional;
    else if constexpr (is_variant<U>::value)
        return Shape::variant;
    else if constexpr (is_tuple<U>::value)
        return Shape::tuple;
    else if constexpr (associative<U> && std::ranges::sized_range<T>)
        return Shape::map;
    else if constexpr (std::ranges::sized_range<T>)
        return Shape::sequence;
    else if constexpr (PagedSource<T>)
        return Shape::paged;
    else if constexpr (HasDescription<U>)
        return Shape::described;
    else if constexpr (formattable<U>)
        return Shape::formatted;
    else
        return Shape::opaque;
}

[[nodiscard]] constexpr std::string_view kindName(Shape shape) noexcept {
    constexpr std::array<std::string_view, 14>
        names{"bool", "integer", "real", "string", "bitset", "optional", "variant", "tuple", "map", "sequence", "sequence", "described", "formatted", "opaque"};
    return names[static_cast<std::size_t>(shape)];
}
}  // namespace detail

/**
 * The kind of a node of type T, in the order of ADR-027 section 4: scalars, strings and bitsets, then optional, variant and
 * tuples, then sized ranges, associative containers and paged sources, then a described type, then a type std::format can
 * write, else `opaque`. The fields of a described type have the kinds of their members, `enum` for a choice.
 */
template <typename T>
[[nodiscard]] constexpr std::string_view kindOf() noexcept {
    return detail::kindName(detail::shapeOf<std::remove_reference_t<T>>());
}

// --- Rendering -------------------------------------------------------------------------------------------------

namespace detail {

/// A reference to a callable that outlives the call it is passed to: what an enumeration of children calls for each.
template <typename Signature>
class FunctionRef;
template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
public:
    template <typename F>
        requires(!std::same_as<std::remove_cvref_t<F>, FunctionRef>) && std::is_invocable_r_v<R, F&, Args...>
    FunctionRef(F&& function) noexcept  // NOLINT(google-explicit-constructor): a callable is passed where one is expected
        : object(const_cast<void*>(static_cast<const void*>(std::addressof(function)))), call([](void* target, Args... args) -> R {
              return (*static_cast<std::remove_reference_t<F>*>(target))(std::forward<Args>(args)...);
          }) { }
    R operator()(Args... args) const {
        return call(object, std::forward<Args>(args)...);
    }

private:
    void* object;
    R (*call)(void*, Args...);
};

struct NodeOps;

/// A node of the object being read: valid only during the call that gives it. `aux` is the field of a described member.
struct NodeRef {
    void*          object = nullptr;
    const NodeOps* ops    = nullptr;
    const void*    aux    = nullptr;
};

/// A child as its parent enumerates it. `rank` is the number of its segment: its position, or the index of the
/// alternative a variant holds.
struct ChildView {
    std::size_t position = 0;
    std::size_t rank     = 0;
    NodeRef     node;
    std::string label;             // visible and bounded
    bool        labelCut = false;  // the label is not the whole one: the node is not writable
};

using ChildFn = FunctionRef<bool(const ChildView&)>;  // false stops the enumeration

/// Thrown by an editor for a text that is not a value of the node, and by an applier that the object refuses.
struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct RefusedError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Writes a value already parsed into the node; the node is the one the editor came from (same type, same field).
using Applier = std::function<void(const NodeRef&)>;
/// Parses a text for a node, without the object: throws ParseError, else gives what writes the value.
using Editor = std::function<Applier(std::string_view)>;

/// How a type is read: one table per type, shared by every node of that type.
struct NodeOps {
    std::string_view (*kind)(const NodeRef&);
    void (*value)(const NodeRef&, TextSink&);                                 // may throw what a std::formatter throws
    std::size_t (*size)(const NodeRef&);                                      // the number of children; may throw what a PagedSource throws
    void (*visit)(const NodeRef&, std::size_t first, std::size_t count, ChildFn each);
    std::optional<std::string> (*key)(const NodeRef&, std::size_t position);  // of a keyed source
    std::optional<std::size_t> (*find)(const NodeRef&, std::string_view key);
    Editor (*editor)(const NodeRef&);                                         // null when values of the type cannot be written
    bool container   = false;                                                 // may have children
    bool keyed       = false;                                                 // its children have keys
    bool alternative = false;                                                 // its one child is told by the rank of an alternative, not by its position
    char letter      = '#';                                                   // of its children's segments
};

template <typename T>
const NodeOps& opsOf() noexcept;

[[nodiscard]] inline std::string typeName(const std::type_info& type) {
#if __has_include(<cxxabi.h>)
    int         status    = 0;
    char*       demangled = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    std::string name{status == 0 && demangled != nullptr ? demangled : type.name()};
    std::free(demangled);  // NOLINT(cppcoreguidelines-no-malloc): the ABI allocates it with malloc
    return name;
#else
    return type.name();
#endif
}

template <std::floating_point F>
void appendNumber(TextSink& sink, F value) {
    std::array<char, 64> digits{};
    const auto           end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    sink.append(std::string_view{digits.data(), static_cast<std::size_t>(end - digits.data())});
}
template <std::integral I>
void appendNumber(TextSink& sink, I value) {
    std::array<char, 24> digits{};
    // A character type is shown as its number: to_chars has no overload for some of them.
    const auto end = std::is_signed_v<I> ? std::to_chars(digits.data(), digits.data() + digits.size(), static_cast<long long>(value)).ptr
                                         : std::to_chars(digits.data(), digits.data() + digits.size(), static_cast<unsigned long long>(value)).ptr;
    sink.append(std::string_view{digits.data(), static_cast<std::size_t>(end - digits.data())});
}

template <typename T>
[[nodiscard]] T& objectOf(const NodeRef& node) noexcept {
    return *static_cast<T*>(node.object);
}

/// A child of type E at `address`; a constant element gives a constant node, which no editor writes.
template <typename E>
[[nodiscard]] NodeRef refOf(E& element) noexcept {
    return {.object = const_cast<void*>(static_cast<const void*>(std::addressof(element))), .ops = &opsOf<E>(), .aux = nullptr};
}

[[nodiscard]] inline ChildView indexedChild(std::size_t position, NodeRef node) {
    return {.position = position, .rank = position, .node = node, .label = '[' + std::to_string(position) + ']', .labelCut = false};
}

/// The description of T, built once.
template <typename T>
[[nodiscard]] const forms::Description<T>& descriptionOf() {
    static const forms::Description<T> description = [] {
        auto built = Described<T>::description();
        forms::validate(built);
        return built;
    }();
    return description;
}

/// The field of a described type T: `aux` is its forms::Field<T>, `object` the T.
template <typename T>
struct FieldRender {
    using Plain = std::remove_const_t<T>;
    static const forms::Field<Plain>& field(const NodeRef& node) noexcept {
        return *static_cast<const forms::Field<Plain>*>(node.aux);
    }
    static std::string_view kind(const NodeRef& node) {
        switch (field(node).kind) {
            case forms::Kind::boolean:
                return "bool";
            case forms::Kind::integer:
                return "integer";
            case forms::Kind::number:
                return "real";
            case forms::Kind::text:
                return "string";
            case forms::Kind::choice:
                return "enum";
        }
        return "opaque";
    }
    static void value(const NodeRef& node, TextSink& sink) {
        sink.append(field(node).read(objectOf<const Plain>(node)));
    }
    static std::size_t size(const NodeRef&) {
        return 0;
    }
    static void visit(const NodeRef&, std::size_t, std::size_t, ChildFn) { }
    /// The text is checked without the object as far as the description tells (kind, bounds, choices); the member's own
    /// type, which only its accessor knows, is checked when the value is written.
    static Editor editor(const NodeRef& node) {
        const auto* described = &field(node);
        return [described](std::string_view text) -> Applier {
            switch (described->kind) {
                case forms::Kind::boolean:
                    if (text != "true" && text != "false")
                        throw ParseError("A boolean is true or false");
                    break;
                case forms::Kind::integer:
                case forms::Kind::number: {
                    const auto number = forms::detail::parseFloating<double>(text);
                    // A text the double parser refuses may still be a whole number beyond 2^53: it is one when the whole
                    // text is read without an error (an empty text and a number too large both stop at its end, with one).
                    const auto wholeAs = [text](auto parsed) {
                        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
                        return error == std::errc{} && end == text.data() + text.size();
                    };
                    const bool exact = wholeAs(0LL) || wholeAs(0ULL);
                    if (!number && !exact)
                        throw ParseError("Not a number");
                    if (described->kind == forms::Kind::integer && !exact && std::trunc(*number) != *number)
                        throw ParseError("Not a whole number");
                    if (number && described->range && (*number < described->range->min || *number > described->range->max))
                        throw ParseError("Out of the bounds " + forms::detail::formatNumber(described->range->min) + " to "
                                         + forms::detail::formatNumber(described->range->max));
                    break;
                }
                case forms::Kind::text:
                    break;
                case forms::Kind::choice:
                    if (std::ranges::find(described->choices, text) == described->choices.end())
                        throw ParseError("Not one of the choices");
                    break;
            }
            return [described, value = std::string(text)](const NodeRef& target) {
                if (!described->write(objectOf<Plain>(target), value))
                    throw RefusedError("The member does not take this value");
            };
        };
    }
    static constexpr NodeOps ops{.kind        = &kind,
                                 .value       = &value,
                                 .size        = &size,
                                 .visit       = &visit,
                                 .key         = nullptr,
                                 .find        = nullptr,
                                 .editor      = std::is_const_v<T> ? nullptr : &editor,
                                 .container   = false,
                                 .keyed       = false,
                                 .alternative = false,
                                 .letter      = '#'};
};

/// How a T is read. T keeps its constness: a constant object has no editor, and its children are constant.
template <typename T>
struct Render {
    using U                      = std::remove_cv_t<T>;
    static constexpr Shape shape = shapeOf<T>();

    static std::string_view kind(const NodeRef&) {
        return kindName(shape);
    }

    static void value(const NodeRef& node, TextSink& sink) {
        [[maybe_unused]] const auto& object = objectOf<const U>(node);
        if constexpr (shape == Shape::boolean)
            sink.append(object ? std::string_view{"true"} : std::string_view{"false"});
        else if constexpr (shape == Shape::integer || shape == Shape::real)
            appendNumber(sink, object);
        else if constexpr (shape == Shape::string)
            sink.append(std::string_view{object});
        else if constexpr (shape == Shape::bitset) {
            for (std::size_t bit = object.size(); bit > 0 && !sink.truncated(); --bit)
                sink.append(object[bit - 1] ? '1' : '0');
        } else if constexpr (shape == Shape::optional)
            sink.append(object ? std::string_view{"engaged"} : std::string_view{"nullopt"});
        else if constexpr (shape == Shape::variant) {
            if (object.valueless_by_exception())
                sink.append(std::string_view{"valueless"});
            else {
                sink.append(std::string_view{"alternative "});
                appendNumber(sink, object.index());
            }
        } else if constexpr (shape == Shape::described)
            sink.append(descriptionOf<U>().name);
        else if constexpr (shape == Shape::formatted)
            sink.format(object);
        else if constexpr (shape == Shape::opaque) {
            sink.append(typeName(typeid(U)));
            sink.append(std::string_view{" (not displayable)"});
        }
        // A tuple or a container shows nothing beside the number of its children, which are its content.
    }

    static std::size_t size(const NodeRef& node) {
        [[maybe_unused]] auto& object = objectOf<T>(node);
        if constexpr (shape == Shape::optional)
            return object ? 1 : 0;
        else if constexpr (shape == Shape::variant)
            return object.valueless_by_exception() ? 0 : 1;
        else if constexpr (shape == Shape::tuple)
            return std::tuple_size_v<U>;
        else if constexpr (shape == Shape::map || shape == Shape::sequence)
            return static_cast<std::size_t>(std::ranges::size(object));
        else if constexpr (shape == Shape::paged)
            return static_cast<std::size_t>(object.size());
        else if constexpr (shape == Shape::described)
            return descriptionOf<U>().fields.size();
        else
            return 0;
    }

    template <std::size_t... I>
    static void visitTuple(T& object, std::size_t first, std::size_t count, ChildFn each, std::index_sequence<I...>) {
        [[maybe_unused]] const auto one = [&](std::size_t index, auto& member) {
            if (index < first || index - first >= count)
                return true;
            ChildView child = indexedChild(index, refOf(member));
            child.label     = std::to_string(index);
            return each(child);
        };
        static_cast<void>((one(I, std::get<I>(object)) && ...));
    }

    template <std::size_t... I>
    static void visitAlternative(T& object, ChildFn each, std::index_sequence<I...>) {
        const auto one = [&](std::size_t index, auto* alternative) {
            if (alternative == nullptr)
                return false;
            ChildView child = indexedChild(0, refOf(*alternative));
            child.rank      = index;
            child.label     = std::to_string(index);
            static_cast<void>(each(child));
            return true;
        };
        static_cast<void>((one(I, std::get_if<I>(&object)) || ...));
    }

    /// One element of a range or of a paged source: an lvalue is read where it is, anything else through a constant copy.
    template <typename Element>
    static bool element(std::size_t position, Element&& item, ChildFn each) {
        if constexpr (std::is_lvalue_reference_v<Element>)
            return elementAt(position, item, each);
        else {
            const std::remove_cvref_t<Element> copy{std::forward<Element>(item)};
            return elementAt(position, copy, each);
        }
    }
    template <typename Item>
    static bool elementAt(std::size_t position, Item& item, ChildFn each) {
        if constexpr (shape == Shape::map) {
            // The node is the mapped value; its label is the key, formatted as a value is.
            TextSink sink{maxTextBytes + 1};
            try {
                opsOf<const typename U::key_type>().value(refOf(item.first), sink);
            } catch (const std::exception& error) {
                sink.append(std::string_view{error.what()});
            }
            auto [label, cut] = visibleText(sink.text(), maxTextBytes, true);
            return each(ChildView{.position = position, .rank = position, .node = refOf(item.second), .label = std::move(label), .labelCut = cut});
        } else
            return each(indexedChild(position, refOf(item)));
    }

    static void visit(const NodeRef& node, std::size_t first, std::size_t count, ChildFn each) {
        [[maybe_unused]] auto& object = objectOf<T>(node);
        if (count == 0)
            return;
        if constexpr (shape == Shape::optional) {
            if (first == 0 && object) {
                ChildView child = indexedChild(0, refOf(*object));
                child.label     = "value";
                static_cast<void>(each(child));
            }
        } else if constexpr (shape == Shape::variant) {
            if (first == 0)
                visitAlternative(object, each, std::make_index_sequence<std::variant_size_v<U>>{});
        } else if constexpr (shape == Shape::tuple)
            visitTuple(object, first, count, each, std::make_index_sequence<std::tuple_size_v<U>>{});
        else if constexpr (shape == Shape::map || shape == Shape::sequence) {
            const auto total = static_cast<std::size_t>(std::ranges::size(object));
            if (first >= total)
                return;
            const auto last = count > total - first ? total : first + count;
            auto       it   = std::ranges::begin(object);
            // A random-access range reaches the position at once; any other is walked from its start.
            std::ranges::advance(it, static_cast<std::ranges::range_difference_t<T>>(first), std::ranges::end(object));
            for (std::size_t position = first; position < last && it != std::ranges::end(object); ++position, ++it)
                if (!element(position, *it, each))
                    return;
        } else if constexpr (shape == Shape::paged) {
            std::size_t position = first;
            bool        more     = true;
            object.visit(first, count, [&](auto&& item) {
                // A source cannot be stopped: what it gives after the page is full, or beyond what was asked, is ignored.
                if (more && position - first < count)
                    more = element(position, std::forward<decltype(item)>(item), each);
                ++position;
            });
        } else if constexpr (shape == Shape::described) {
            const auto& fields = descriptionOf<U>().fields;
            for (std::size_t rank = first; rank < fields.size() && rank - first < count; ++rank) {
                auto [label, cut] = visibleText(fields[rank].label, maxTextBytes, true);
                if (!each(ChildView{
                        .position = rank,
                        .rank     = rank,
                        .node     = NodeRef{.object = node.object, .ops = &FieldRender<T>::ops, .aux = &fields[rank]},
                        .label    = std::move(label),
                        .labelCut = cut
                }))
                    return;
            }
        } else
            static_cast<void>(first), static_cast<void>(each);
    }

    static std::optional<std::string> key(const NodeRef& node, std::size_t position) {
        if constexpr (KeyedPagedSource<T>)
            return std::string(objectOf<T>(node).key(position));
        else
            return static_cast<void>(node), static_cast<void>(position), std::nullopt;
    }
    static std::optional<std::size_t> find(const NodeRef& node, std::string_view wanted) {
        if constexpr (KeyedPagedSource<T>)
            return objectOf<T>(node).find(wanted);
        else
            return static_cast<void>(node), static_cast<void>(wanted), std::nullopt;
    }

    static Editor editor(const NodeRef&) {
        return [](std::string_view input) -> Applier {
            if constexpr (shape == Shape::boolean) {
                if (input != "true" && input != "false")
                    throw ParseError("A boolean is true or false");
                return [value = input == "true"](const NodeRef& target) {
                    objectOf<U>(target) = value;
                };
            } else if constexpr (shape == Shape::integer || shape == Shape::real) {
                const auto parsed = forms::detail::parseNumber<U>(input);
                if (!parsed)
                    throw ParseError(std::is_floating_point_v<U> ? "Not a finite number" : "Not a whole number of the value's type");
                return [value = *parsed](const NodeRef& target) {
                    objectOf<U>(target) = value;
                };
            } else if constexpr (std::is_same_v<U, std::string>) {
                return [value = std::string(input)](const NodeRef& target) {
                    objectOf<U>(target) = value;
                };
            } else if constexpr (shape == Shape::bitset) {
                if (input.size() != U{}.size() || input.find_first_not_of("01") != std::string_view::npos)
                    throw ParseError("A bitset is written as " + std::to_string(U{}.size()) + " binary digits");
                U value;
                for (std::size_t bit = 0; bit < input.size(); ++bit)
                    value[input.size() - 1 - bit] = input[bit] == '1';
                return [value](const NodeRef& target) {
                    objectOf<U>(target) = value;
                };
            } else
                return static_cast<void>(input), Applier{};
        };
    }

    static constexpr bool editable    = !std::is_const_v<T>
                                        && (shape == Shape::boolean || shape == Shape::integer || shape == Shape::real || shape == Shape::bitset
                                            || std::is_same_v<U, std::string>);
    static constexpr bool hasChildren = shape == Shape::optional || shape == Shape::variant || shape == Shape::tuple || shape == Shape::map
                                        || shape == Shape::sequence || shape == Shape::paged || shape == Shape::described;

    static constexpr NodeOps ops{.kind        = &kind,
                                 .value       = &value,
                                 .size        = &size,
                                 .visit       = &visit,
                                 .key         = &key,
                                 .find        = &find,
                                 .editor      = editable ? &editor : nullptr,
                                 .container   = hasChildren,
                                 .keyed       = KeyedPagedSource<T>,
                                 .alternative = shape == Shape::variant,
                                 .letter      = shape == Shape::described ? '.' : '#'};
};

template <typename T>
const NodeOps& opsOf() noexcept {
    return Render<std::remove_volatile_t<T>>::ops;
}

}  // namespace detail

/**
 * @brief What a watch's visitor source receives: WebFront calls the source with a Visit, and the source calls it once with
 * the object, under its own lock, so that the whole operation runs while the object is safe to read. `Visit<const T>`
 * only reads; `Visit<T>` makes the watch writable.
 */
template <typename T>
class Visit {
public:
    using Object = T;

    explicit Visit(std::function<void(T&)> visitor) : run(std::move(visitor)) { }

    void operator()(T& object) const {
        run(object);
    }

private:
    std::function<void(T&)> run;
};

}  // namespace webfront::inspect
