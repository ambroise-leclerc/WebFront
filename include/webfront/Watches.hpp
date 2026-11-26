// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — core
// Role: The registry of the watches an application declares for the inspectors (ADR-027), and its discovery page.
// Thread: thread-safe | Alloc: heap-init
// LLM: public API (Watch, Visit) and the bounds of ADR-027 section 3; the registry is read by webfront.inspector.watches

#pragma once

#include "http/BuffersPolicy.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace webfront::inspect {

// --- Bounds (ADR-027, section 3) -------------------------------------------------------------------------------

/// A watch's name, and the texts of a descriptor (name, location), are at most this many UTF-8 bytes.
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

/// What an answer may take with the policy: it must fit both the encoding buffer and the message size.
template <typename Policy>
inline constexpr std::size_t responseBudget = std::min<std::size_t>(Policy::functionCallBufferSize, http::maxMessageSize<Policy>());

/// The error codes of the inspectors (typed codes of ADR-003): the caller is not admitted, or names a watch that is gone.
namespace code {
inline constexpr std::string_view denied      = "webfront.inspector.denied";
inline constexpr std::string_view unknown     = "webfront.inspector.unknown";
inline constexpr std::string_view unavailable = "webfront.inspector.unavailable";
}  // namespace code

/// The prefix of the names reserved to the inspectors: cppFunction refuses them to the application (ADR-027, section 1).
inline constexpr std::string_view reservedPrefix = "webfront.inspector.";

[[nodiscard]] inline bool isReservedName(std::string_view name) noexcept {
    return name.starts_with(reservedPrefix);
}

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
}  // namespace detail

/// A text of the application made safe to show and bounded: control characters become their visible pictures (U+2400
/// to U+241F, U+2421 for DEL), then the text is cut to `limit` bytes on a character boundary, ending with "…" when cut.
/// A limit too small for the ellipsis gives the longest prefix that fits, without it.
[[nodiscard]] inline std::string boundedText(std::string_view text, std::size_t limit = maxTextBytes) {
    std::string visible;
    visible.reserve((std::min)(text.size(), limit + 4));  // parenthesized: Windows headers may define min
    for (const char c : text) {
        detail::appendVisible(visible, c);
        if (visible.size() > limit + 4)
            break;  // enough to know that it is cut
    }
    if (visible.size() <= limit)
        return visible;
    if (limit < detail::ellipsis.size())
        return std::string{utf8Prefix(visible, limit)};
    std::string cut{utf8Prefix(visible, limit - detail::ellipsis.size())};
    cut += detail::ellipsis;
    return cut;
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
}  // namespace detail

/**
 * The kind of a node of type T, in the order of ADR-027 section 4: scalars, strings and bitsets, then optional, variant and
 * tuples, then sized ranges and associative containers, then a type std::format can write, else `opaque`. Enumerations and
 * described types (`enum`, `described`) are recognized once a description is given to the watch.
 */
template <typename T>
[[nodiscard]] constexpr std::string_view kindOf() noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>)
        return "bool";
    else if constexpr (std::is_integral_v<U>)
        return "integer";
    else if constexpr (std::is_floating_point_v<U>)
        return "real";
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>)
        return "string";
    else if constexpr (detail::is_bitset<U>::value)
        return "bitset";
    else if constexpr (detail::is_optional<U>::value)
        return "optional";
    else if constexpr (detail::is_variant<U>::value)
        return "variant";
    else if constexpr (detail::is_tuple<U>::value)
        return "tuple";
    else if constexpr (detail::associative<U>)
        return "map";
    else if constexpr (std::ranges::sized_range<U>)
        return "sequence";
    else if constexpr (std::formattable<U, char>)
        return "formatted";
    else
        return "opaque";
}

// --- Sources (ADR-027, section 5) ------------------------------------------------------------------------------

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

namespace detail {
template <typename T>
struct visit_object { };
template <typename T>
struct visit_object<Visit<T>> {
    using type = T;
};

template <typename Signature>
struct single_parameter { };
template <typename R, typename A>
struct single_parameter<R (*)(A)> {
    using type = A;
};
template <typename R, typename A>
struct single_parameter<R (*)(A) noexcept> : single_parameter<R (*)(A)> { };
template <typename R, typename C, typename A>
struct single_parameter<R (C::*)(A)> : single_parameter<R (*)(A)> { };
template <typename R, typename C, typename A>
struct single_parameter<R (C::*)(A) const> : single_parameter<R (*)(A)> { };
template <typename R, typename C, typename A>
struct single_parameter<R (C::*)(A) noexcept> : single_parameter<R (*)(A)> { };
template <typename R, typename C, typename A>
struct single_parameter<R (C::*)(A) const noexcept> : single_parameter<R (*)(A)> { };

template <typename Source>
struct call_signature {
    using type = Source;  // a function pointer
};
template <typename Source>
    requires requires { &Source::operator(); }
struct call_signature<Source> {
    using type = decltype(&Source::operator());
};

/// The object a visitor source visits, when Source is one: a callable whose one parameter is a Visit<T>.
template <typename Source>
concept visitor_source = requires { typename visit_object<std::remove_cvref_t<typename single_parameter<typename call_signature<Source>::type>::type>>::type; };
}  // namespace detail

/**
 * @brief How a watch reaches its object, deduced from its source: a visitor (`[&](Visit<const T> visit) { ... visit(object); }`),
 * or an accessor that returns a reference or a copy (`[&] -> const T& { return object; }`). Only a visitor of a non-constant
 * object is writable: a reference read on the server thread is not, and a copy is not the object (ADR-027, section 6).
 */
template <typename Source>
struct SourceShape;

template <typename Source>
    requires detail::visitor_source<Source>
struct SourceShape<Source> {
    using Object =
        typename detail::visit_object<std::remove_cvref_t<typename detail::single_parameter<typename detail::call_signature<Source>::type>::type>>::type;
    static constexpr bool visitor  = true;
    static constexpr bool writable = !std::is_const_v<Object>;
};

template <typename Source>
    requires(!detail::visitor_source<Source> && std::invocable<Source&>)
struct SourceShape<Source> {
    using Object                   = std::remove_reference_t<std::invoke_result_t<Source&>>;
    static constexpr bool visitor  = false;
    static constexpr bool writable = false;
};

template <typename Source>
concept WatchSource = requires { typename SourceShape<std::decay_t<Source>>::Object; };

// --- Registry --------------------------------------------------------------------------------------------------

/// A watch as webfront.inspector.watches describes it: (identifier, name, kind of the root, "file:line", writable).
using Descriptor = std::tuple<double, std::string, std::string, std::string, bool>;
/// A page of descriptors: (descriptors, next, revision of the registry, changed). `next` is 0 at the end.
using WatchPage = std::tuple<std::vector<Descriptor>, double, double, bool>;

/// The largest number a page exchanges exactly with JavaScript (2^53).
inline constexpr double maxExactNumber = 9007199254740992.0;

/**
 * @brief The watches of an application (ADR-027, section 3, "Découverte"). Each registration gets an identifier that is
 * never reused while the registry lives, and the registry's revision changes at each registration and each removal.
 * Reading it calls no source: it is a read of the table. Thread-safe.
 */
class WatchRegistry {
public:
    struct Entry {
        std::string           name;   // as the application gave it: what makes a watch unique
        std::string           label;  // the name as the inspectors show it: control characters visible, bounded (boundedText)
        std::string           kind;
        std::string           location;
        bool                  writable = false;
        std::shared_ptr<void> source;  // what the source captures lives as long as its registration
    };

    /// Registers a watch; throws std::invalid_argument for an empty name, a name longer than maxTextBytes, a kind longer
    /// than maxKindBytes, or the name of a watch that is still registered. The location is made visible and bounded as a
    /// text of the application is, so that every descriptor fits a page of the smallest budget.
    std::uint64_t add(std::string name, std::string_view kind, std::string location, bool writable, std::shared_ptr<void> source) {
        checkName(name);
        if (kind.empty() || kind.size() > maxKindBytes)
            throw std::invalid_argument("A watch's kind is 1 to 16 bytes");
        auto             label   = boundedText(name);
        auto             shown   = boundedText(location);
        auto             keeping = source;  // declared before the lock: a source released by a failed insertion is destroyed after it
        std::scoped_lock lock{mutex};
        for (const auto& [id, entry] : entries)
            if (entry.name == name)
                throw std::invalid_argument("A watch named '" + name + "' is already registered");
        const auto id = nextId++;
        entries.emplace(id, Entry{std::move(name), std::move(label), std::string(kind), std::move(shown), writable, std::move(source)});
        ++currentRevision;
        return id;
    }

    /// Removes a watch; a removed or unknown identifier is ignored. The source is released outside the registry's lock.
    void remove(std::uint64_t id) noexcept {
        std::shared_ptr<void> released;
        {
            std::scoped_lock lock{mutex};
            const auto       found = entries.find(id);
            if (found == entries.end())
                return;
            released = std::move(found->second.source);
            entries.erase(found);
            ++currentRevision;
        }
    }

    [[nodiscard]] std::uint64_t revision() const {
        std::scoped_lock lock{mutex};
        return currentRevision;
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock{mutex};
        return entries.size();
    }

    [[nodiscard]] bool contains(std::uint64_t id) const {
        std::scoped_lock lock{mutex};
        return entries.contains(id);
    }

    /**
     * The page that webfront.inspector.watches(revision, after, count) answers: the live watches whose identifier is
     * greater than `after`, in the order of the identifiers, at most `count` of them (0 asks only for the revision) and as
     * many as fit `budget` bytes with the answer's header. `next` is the last identifier served, 0 at the end; `changed`
     * tells that `revision` is no longer the registry's. Throws std::invalid_argument for a number that is not a whole
     * number from 0 to 2^53, and std::length_error when the first remaining watch does not fit `budget`: an empty page
     * ending with 0 would hide the rest (a budget of minPageBytes always holds one).
     */
    [[nodiscard]] WatchPage page(double revision, double after, double count, std::size_t budget) const {
        const auto              known = wholeNumber(revision, "revision"), from = wholeNumber(after, "cursor");
        const auto              wanted = std::min<std::uint64_t>(wholeNumber(count, "count"), maxPageCount);
        const auto              room   = budget > responseHeaderBytes ? budget - responseHeaderBytes : 0;
        std::scoped_lock        lock{mutex};
        std::vector<Descriptor> served;
        std::size_t             used = 0;
        std::uint64_t           last = 0;
        bool                    more = false;
        for (auto entry = entries.upper_bound(from); entry != entries.end(); ++entry) {
            const auto bytes = encodedSize(entry->second);
            if (served.size() == wanted || used + bytes > room) {
                if (served.empty() && wanted != 0)
                    throw std::length_error("The inspector's page budget cannot hold one watch descriptor");
                more = true;
                break;
            }
            used += bytes;
            last  = entry->first;
            served.emplace_back(static_cast<double>(entry->first), entry->second.label, entry->second.kind, entry->second.location, entry->second.writable);
        }
        const bool changed = known != currentRevision;
        return {std::move(served), more && wanted != 0 ? static_cast<double>(last) : 0.0, static_cast<double>(currentRevision), changed};
    }

    /// The largest number of bytes a descriptor takes encoded: the bound that makes a page of one always fit minPageBytes.
    static constexpr std::size_t maxDescriptorBytes = 2 + 9 + (3 + maxTextBytes) + (3 + maxKindBytes) + (3 + maxTextBytes) + 1;
    static_assert(maxDescriptorBytes + responseHeaderBytes <= minPageBytes);

    static void checkName(std::string_view name) {
        if (name.empty() || name.size() > maxTextBytes)
            throw std::invalid_argument("A watch's name is 1 to 256 bytes");
    }

private:
    /// An upper bound of the encoded size of a descriptor: a tuple header, a number, three strings with the longest string
    /// header and a boolean.
    static std::size_t encodedSize(const Entry& entry) noexcept {
        return 2 + 9 + (3 + entry.label.size()) + (3 + entry.kind.size()) + (3 + entry.location.size()) + 1;
    }

    static std::uint64_t wholeNumber(double value, std::string_view what) {
        if (!std::isfinite(value) || value < 0 || value > maxExactNumber || std::trunc(value) != value)
            throw std::invalid_argument("The watches' " + std::string(what) + " must be a whole number from 0 to 2^53");
        return static_cast<std::uint64_t>(value);
    }

    mutable std::mutex             mutex;
    std::map<std::uint64_t, Entry> entries;
    std::uint64_t                  nextId          = 1;
    std::uint64_t                  currentRevision = 1;  // never 0: an inspector that starts with 0 sees a change
};

/**
 * @brief The handle of a watch, as UISubscription is the handle of a subscription: the watch exists while it lives, and
 * the handle stays safe after the application is destroyed. An empty handle registered nothing: outside the developer
 * environment, and in a distribution build, `app.watch` returns one (ADR-027, section 1). Move-only.
 */
class Watch {
public:
    Watch() = default;
    Watch(std::weak_ptr<WatchRegistry> owner, std::uint64_t identifier) : registry(std::move(owner)), watchId(identifier) { }
    Watch(const Watch&)            = delete;
    Watch& operator=(const Watch&) = delete;
    Watch(Watch&& other) noexcept : registry(std::move(other.registry)), watchId(std::exchange(other.watchId, 0)) { }
    Watch& operator=(Watch&& other) noexcept {
        if (this != &other) {
            // The previous watch is removed once this handle holds the new one: its source, released then, may assign
            // this handle again, and that assignment is the one kept.
            Watch previous{std::move(*this)};
            registry = std::move(other.registry);
            watchId  = std::exchange(other.watchId, 0);
        }
        return *this;
    }
    ~Watch() {
        reset();
    }

    /// Removes the watch now; the handle becomes empty. The handle is emptied before the removal and not touched after it:
    /// the source, released by the removal, may assign this handle to a new watch, which stays registered.
    void reset() noexcept {
        const auto id    = std::exchange(watchId, 0);
        const auto owner = std::exchange(registry, {}).lock();
        if (id != 0 && owner)
            owner->remove(id);
    }

    /// The identifier the inspectors know the watch by, 0 for an empty handle.
    [[nodiscard]] std::uint64_t id() const noexcept {
        return watchId;
    }

    explicit operator bool() const noexcept {
        return watchId != 0;
    }

private:
    std::weak_ptr<WatchRegistry> registry;
    std::uint64_t                watchId = 0;
};

}  // namespace webfront::inspect
