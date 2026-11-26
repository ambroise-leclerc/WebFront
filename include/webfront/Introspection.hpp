// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-26
//
// WebFront — core
// Role: What an application registered and runs, kept for the inspectors (ADR-027, section 7): C++ functions, JavaScript calls, browsers.
// Thread: thread-safe | Alloc: heap-init
// LLM: internal; the counters are relaxed atomics on the call path, read the cost notes before adding work there

#pragma once

#include "Watches.hpp"
#include "weblink/Messages.hpp"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::inspect {

/// False in a distribution build (WEBFRONT_DISTRIBUTION): nothing is recorded there, and no call is counted.
#if defined(WEBFRONT_DISTRIBUTION) && WEBFRONT_DISTRIBUTION
inline constexpr bool introspection = false;
#else
inline constexpr bool introspection = true;
#endif

/// A signature is written in at most this many bytes; a longer one ends with "…".
inline constexpr std::size_t maxSignatureBytes = 512;
/// The names of the JavaScript functions that C++ calls are counted one by one up to this many; the calls of the names
/// that follow are counted together, so that an application that builds its names cannot grow the table without bound.
inline constexpr std::size_t maxJsFunctionNames = 1024;
/// A duration is measured while an inspector read the functions less than this long ago.
inline constexpr std::chrono::seconds timingWindow{5};

// --- TypeScript names ------------------------------------------------------------------------------------------

namespace detail {
template <typename T>
struct typescript_tuple : std::false_type { };

template <typename T>
[[nodiscard]] std::string typescriptName();

template <typename... T>
struct typescript_tuple<std::tuple<T...>> : std::true_type {
    static std::string write() {
        std::string text = "[";
        static_cast<void>(((text += (text.size() > 1 ? ", " : "") + typescriptName<T>()), ...));
        return text + "]";
    }
};
template <typename A, typename B>
struct typescript_tuple<std::pair<A, B>> : typescript_tuple<std::tuple<A, B>> { };

template <typename Element>
[[nodiscard]] constexpr std::string_view typedArrayName() noexcept {
    using E = std::remove_cv_t<Element>;
    if constexpr (std::is_same_v<E, std::uint8_t>)
        return "Uint8Array";
    else if constexpr (std::is_same_v<E, std::int8_t>)
        return "Int8Array";
    else if constexpr (std::is_same_v<E, std::uint16_t>)
        return "Uint16Array";
    else if constexpr (std::is_same_v<E, std::int16_t>)
        return "Int16Array";
    else if constexpr (std::is_same_v<E, std::uint32_t>)
        return "Uint32Array";
    else if constexpr (std::is_same_v<E, std::int32_t>)
        return "Int32Array";
    else if constexpr (std::is_same_v<E, std::uint64_t>)
        return "BigUint64Array";
    else if constexpr (std::is_same_v<E, std::int64_t>)
        return "BigInt64Array";
    else if constexpr (std::is_same_v<E, float>)
        return "Float32Array";
    else
        return "Float64Array";
}

template <typename T>
[[nodiscard]] std::string typescriptName() {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_void_v<U>)
        return "void";
    else if constexpr (std::is_same_v<U, bool>)
        return "boolean";
    else if constexpr (std::is_arithmetic_v<U>)
        return "number";
    else if constexpr (std::is_convertible_v<U, std::string_view>)
        return "string";
    else if constexpr (msg::is_bitset_v<U>)
        return "Uint8Array";
    else if constexpr (msg::is_numeric_array_v<U>)
        return std::string{typedArrayName<msg::numeric_array_element_t<U>>()};
    else if constexpr (typescript_tuple<U>::value)
        return typescript_tuple<U>::write();
    else if constexpr (msg::is_vector_v<U>)
        return typescriptName<typename U::value_type>() + "[]";
    else
        return "unknown";
}
}  // namespace detail

/**
 * The TypeScript type that JavaScript gives or receives for the C++ type T on the bridge: `boolean`, `number`, `string`,
 * the typed array of a vector, array or span of numbers (`Uint8Array` for a `std::bitset`, packed), `[A, B]` for a tuple
 * or a pair, `T[]` for a list, `void`, and `unknown` for a type the bridge does not carry.
 */
template <typename T>
[[nodiscard]] std::string typescriptType() {
    return detail::typescriptName<T>();
}

/// The signature of a function as a page declares it: `(a: number, b: string) => Promise<string>`. A call from JavaScript
/// always returns a Promise, also for a function that returns nothing.
[[nodiscard]] inline std::string typescriptSignature(const std::vector<std::string>& parameters, std::string_view result) {
    std::string text = "(";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0)
            text += ", ";
        // a, b, ... z, then a1, b1, ...: a name the declaration needs and the C++ signature does not give
        text += static_cast<char>('a' + i % 26);
        if (i >= 26)
            text += std::to_string(i / 26);
        text += ": " + parameters[i];
    }
    return boundedText(text + ") => Promise<" + std::string(result) + ">", maxSignatureBytes);
}

// --- Numbers of a page -----------------------------------------------------------------------------------------

namespace detail {
/// A cursor or a count given by a page: a whole number from 0 to 2^53, else std::invalid_argument.
[[nodiscard]] inline std::uint64_t pageNumber(double value, std::string_view what) {
    if (!std::isfinite(value) || value < 0 || value > maxExactNumber || std::trunc(value) != value)
        throw std::invalid_argument("The inspectors' " + std::string(what) + " must be a whole number from 0 to 2^53");
    return static_cast<std::uint64_t>(value);
}

/// The bytes an encoded string takes at most: its header (a 16-bit length for these bounded texts) and its bytes.
[[nodiscard]] constexpr std::size_t encodedText(std::size_t bytes) noexcept {
    return 3 + bytes;
}
inline constexpr std::size_t encodedNumber = 9;
inline constexpr std::size_t encodedTuple  = 2;
/// What a page may give to its rows: the budget without the answer's header.
[[nodiscard]] constexpr std::size_t rowsRoom(std::size_t budget) noexcept {
    return budget > responseHeaderBytes ? budget - responseHeaderBytes : 0;
}
}  // namespace detail

// --- C++ functions ---------------------------------------------------------------------------------------------

/**
 * @brief Whether the durations of the calls are measured: only while an inspector reads the functions, so that an
 * application nobody inspects reads no clock on a call. A read opens the gate for timingWindow; the first call after the
 * window, which reads the clock anyway, closes it.
 */
class TimingGate {
public:
    using Clock = std::chrono::steady_clock;

    /// An inspector read the functions: the calls are timed for `window` from now.
    void observe(Clock::duration window = timingWindow) noexcept {
        until.store((Clock::now() + window).time_since_epoch().count(), std::memory_order_relaxed);
        open.store(true, std::memory_order_relaxed);
    }

    /// The start of a call to time, or nothing when no inspector is reading: one relaxed load in that case.
    [[nodiscard]] std::optional<Clock::time_point> begin() noexcept {
        if (!open.load(std::memory_order_relaxed))
            return std::nullopt;
        const auto now = Clock::now();
        if (now.time_since_epoch().count() > until.load(std::memory_order_relaxed)) {
            // A read that opens the gate at this very moment may be undone: its next read, a period later, opens it again.
            open.store(false, std::memory_order_relaxed);
            return std::nullopt;
        }
        return now;
    }

    [[nodiscard]] bool isOpen() const noexcept {
        return open.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool>       open{false};
    std::atomic<Clock::rep> until{0};
};

/// What is kept of one registered C++ function: what it is, and what happened to its calls. Owned by its handler: the
/// record of a function that is released (a space's, with its space) goes with it.
struct FunctionRecord {
    std::uint64_t               id = 0;
    std::string                 name;                 // as it was registered
    std::string                 label;                // the name as the inspectors show it (boundedText)
    std::string                 signature;            // TypeScript, bounded
    std::string                 location;             // "file:line" of the registration, bounded
    std::string                 scope;                // the path of the space that owns it, empty for the application's
    bool                        takesCaller = false;  // its first parameter is the calling UI
    bool                        deferred    = false;  // it returns a sender (a ResultTask): the answer comes when it completes
    std::shared_ptr<TimingGate> gate;

    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> failures{0};
    std::atomic<std::uint64_t> timedCalls{0};
    std::atomic<std::uint64_t> totalNanoseconds{0};
    std::atomic<std::uint64_t> maxNanoseconds{0};

    /// A call failed: counted, and its message kept, bounded and made visible. Never throws.
    void fail(std::string_view message) noexcept {
        failures.fetch_add(1, std::memory_order_relaxed);
        try {
            auto             text = boundedText(message);
            std::scoped_lock lock{errorMutex};
            error = std::move(text);
        } catch (...) { }  // without memory for the message, the count alone tells the failure
    }

    void timed(TimingGate::Clock::duration duration) noexcept {
        const auto nanoseconds = static_cast<std::uint64_t>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));
        timedCalls.fetch_add(1, std::memory_order_relaxed);
        totalNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
        auto longest = maxNanoseconds.load(std::memory_order_relaxed);
        while (nanoseconds > longest && !maxNanoseconds.compare_exchange_weak(longest, nanoseconds, std::memory_order_relaxed)) { }
    }

    [[nodiscard]] std::string lastError() const {
        std::scoped_lock lock{errorMutex};
        return error;
    }

private:
    mutable std::mutex errorMutex;
    std::string        error;
};

/**
 * @brief One call of a registered function: counted when it starts, timed until it ends when an inspector is reading.
 * Without a record (a distribution build, a function of WebFront's own inspectors) it does nothing. Move-only, so that a
 * call answered later carries it to its answer.
 */
class CallMeasure {
public:
    CallMeasure() = default;
    /// A call that ends before its handler returns: the handler owns the record meanwhile, and the measure shares nothing
    /// (no reference count is touched on the call path).
    explicit CallMeasure(FunctionRecord* function) noexcept : record(function) {
        begin();
    }
    /// A call answered later: the measure keeps the record until the answer, even if the function is released meanwhile.
    explicit CallMeasure(std::shared_ptr<FunctionRecord> function) noexcept : record(function.get()), owner(std::move(function)) {
        begin();
    }
    CallMeasure(CallMeasure&& other) noexcept
        : record(std::exchange(other.record, nullptr)), owner(std::move(other.owner)), start(std::exchange(other.start, std::nullopt)) { }
    CallMeasure& operator=(CallMeasure&& other) noexcept {
        if (this != &other) {
            finish();
            record = std::exchange(other.record, nullptr);
            owner  = std::move(other.owner);
            start  = std::exchange(other.start, std::nullopt);
        }
        return *this;
    }
    CallMeasure(const CallMeasure&)            = delete;
    CallMeasure& operator=(const CallMeasure&) = delete;
    ~CallMeasure() {
        finish();
    }

    /// The call failed with this message (an exception's, or what stopped it).
    void failed(std::string_view message) const noexcept {
        if (record)
            record->fail(message);
    }

private:
    void begin() noexcept {
        if (!record)
            return;
        record->calls.fetch_add(1, std::memory_order_relaxed);
        if (record->gate)
            start = record->gate->begin();
    }

    void finish() noexcept {
        if (record && start)
            record->timed(TimingGate::Clock::now() - *start);
        record = nullptr;
        owner.reset();
        start.reset();
    }

    FunctionRecord*                              record = nullptr;
    std::shared_ptr<FunctionRecord>              owner;
    std::optional<TimingGate::Clock::time_point> start;
};

/// A registered function as the application reads it (BasicWF::registeredFunctions): a copy, taken at one moment.
struct FunctionInfo {
    std::uint64_t            id = 0;
    std::string              name;
    std::string              signature;
    std::string              location;
    std::string              scope;
    bool                     takesCaller = false;
    bool                     deferred    = false;
    std::uint64_t            calls       = 0;
    std::uint64_t            failures    = 0;
    std::string              lastError;
    std::uint64_t            timedCalls = 0;
    std::chrono::nanoseconds totalDuration{0};
    std::chrono::nanoseconds maxDuration{0};
};

/// The bits of a function's flags, in a row of webfront.inspector.functions.
namespace function_flag {
inline constexpr unsigned takesCaller = 1;  // receives the calling UI: it can check ui.isLocalClient() (ADR-027, section 2)
inline constexpr unsigned deferred    = 2;  // answers when the sender it returned completes
inline constexpr unsigned timed       = 4;  // at least one of its calls was timed: the durations mean something
}  // namespace function_flag

/// A function as webfront.inspector.functions describes it: (identifier, name, signature, "file:line", space, flags,
/// calls, failures, last error, mean duration in milliseconds, longest duration in milliseconds).
using FunctionRow = std::tuple<double, std::string, std::string, std::string, std::string, double, double, double, std::string, double, double>;
/// A page of functions: (rows, next). `next` is the cursor of the following page, 0 at the end.
using FunctionPage = std::tuple<std::vector<FunctionRow>, double>;

/**
 * @brief The C++ functions of an application, as cppFunction registered them. A record lives as long as its handler: the
 * registry only lists those that still do. Thread-safe.
 */
class FunctionRegistry {
public:
    /// Records a function about to be registered. The caller keeps the record with the handler; a registration that is
    /// refused (the name is taken) releases it, and it is listed no more.
    [[nodiscard]] std::shared_ptr<FunctionRecord>
    add(std::string_view name, std::string signature, bool takesCaller, bool deferred, const std::source_location& location, std::string_view scope = {}) {
        auto record         = std::make_shared<FunctionRecord>();
        record->name        = std::string(name);
        record->label       = boundedText(name);
        record->signature   = std::move(signature);
        record->location    = locationText(location);
        record->scope       = boundedText(scope);
        record->takesCaller = takesCaller;
        record->deferred    = deferred;
        record->gate        = gate;
        std::scoped_lock lock{mutex};
        record->id = nextId++;
        entries.emplace(record->id, record);
        return record;
    }

    /// The functions that are still registered, in the order of their registration.
    [[nodiscard]] std::vector<FunctionInfo> list() const {
        std::vector<FunctionInfo> functions;
        for (const auto& record : alive(0))
            functions.push_back(snapshot(*record));
        return functions;
    }

    /// The function registered under that name (and in that space), if it still is.
    [[nodiscard]] std::optional<FunctionInfo> find(std::string_view name, std::string_view scope = {}) const {
        for (const auto& record : alive(0))
            if (record->name == name && record->scope == scope)
                return snapshot(*record);
        return std::nullopt;
    }

    /**
     * The page that webfront.inspector.functions(after, count) answers: the functions whose identifier is greater than
     * `after`, in the order of the identifiers, at most `count` (1 000 at most) and as many as fit `budget` bytes. Reading
     * it tells that an inspector is open: the calls are timed for timingWindow. Throws std::invalid_argument for a number
     * that is not a whole number from 0 to 2^53.
     */
    [[nodiscard]] FunctionPage page(double after, double count, std::size_t budget) const {
        const auto from   = detail::pageNumber(after, "cursor");
        const auto wanted = std::min<std::uint64_t>(detail::pageNumber(count, "count"), maxPageCount);
        const auto room   = detail::rowsRoom(budget);
        gate->observe();
        std::vector<FunctionRow> rows;
        std::size_t              used = 0;
        std::uint64_t            last = 0;
        bool                     more = false;
        for (const auto& record : alive(from)) {
            auto       info  = snapshot(*record);
            const auto bytes = encodedSize(info);
            if (rows.size() == wanted || used + bytes > room) {
                more = true;
                break;
            }
            used += bytes;
            last  = info.id;
            rows.push_back(row(info));
        }
        return {std::move(rows), more && last != 0 ? static_cast<double>(last) : 0.0};
    }

    /// Opens or closes the timing as a read of the functions does, for `window` (a test closes it with a negative one).
    void observe(TimingGate::Clock::duration window = timingWindow) noexcept {
        gate->observe(window);
    }

    [[nodiscard]] bool timing() const noexcept {
        return gate->isOpen();
    }

    /// The largest number of bytes a row takes encoded: a page of one always fits minPageBytes.
    static constexpr std::size_t maxRowBytes = detail::encodedTuple + 6 * detail::encodedNumber + 4 * detail::encodedText(maxTextBytes)
                                               + detail::encodedText(maxSignatureBytes);
    static_assert(maxRowBytes + responseHeaderBytes <= minPageBytes);

    [[nodiscard]] static FunctionRow row(const FunctionInfo& info) {
        const auto milliseconds = [](std::chrono::nanoseconds duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        const unsigned flags = (info.takesCaller ? function_flag::takesCaller : 0u) | (info.deferred ? function_flag::deferred : 0u)
                               | (info.timedCalls != 0 ? function_flag::timed : 0u);
        const double   mean  = info.timedCalls != 0 ? milliseconds(info.totalDuration) / static_cast<double>(info.timedCalls) : 0.0;
        return {static_cast<double>(info.id),
                info.name,
                info.signature,
                info.location,
                info.scope,
                static_cast<double>(flags),
                static_cast<double>(info.calls),
                static_cast<double>(info.failures),
                info.lastError,
                mean,
                milliseconds(info.maxDuration)};
    }

private:
    /// The records still owned by a handler whose identifier is greater than `after`; those released are forgotten.
    [[nodiscard]] std::vector<std::shared_ptr<FunctionRecord>> alive(std::uint64_t after) const {
        std::vector<std::shared_ptr<FunctionRecord>> records;
        std::scoped_lock                             lock{mutex};
        for (auto entry = entries.begin(); entry != entries.end();) {
            if (auto record = entry->second.lock()) {
                if (entry->first > after)
                    records.push_back(std::move(record));
                ++entry;
            } else
                entry = entries.erase(entry);
        }
        return records;
    }

    [[nodiscard]] static FunctionInfo snapshot(const FunctionRecord& record) {
        FunctionInfo info;
        info.id            = record.id;
        info.name          = record.label;
        info.signature     = record.signature;
        info.location      = record.location;
        info.scope         = record.scope;
        info.takesCaller   = record.takesCaller;
        info.deferred      = record.deferred;
        info.calls         = record.calls.load(std::memory_order_relaxed);
        info.failures      = record.failures.load(std::memory_order_relaxed);
        info.lastError     = record.lastError();
        info.timedCalls    = record.timedCalls.load(std::memory_order_relaxed);
        info.totalDuration = std::chrono::nanoseconds{static_cast<std::int64_t>(record.totalNanoseconds.load(std::memory_order_relaxed))};
        info.maxDuration   = std::chrono::nanoseconds{static_cast<std::int64_t>(record.maxNanoseconds.load(std::memory_order_relaxed))};
        return info;
    }

    [[nodiscard]] static std::size_t encodedSize(const FunctionInfo& info) noexcept {
        return detail::encodedTuple + 6 * detail::encodedNumber + detail::encodedText(info.name.size()) + detail::encodedText(info.signature.size())
               + detail::encodedText(info.location.size()) + detail::encodedText(info.scope.size()) + detail::encodedText(info.lastError.size());
    }

    std::shared_ptr<TimingGate>                                    gate = std::make_shared<TimingGate>();
    mutable std::mutex                                             mutex;
    mutable std::map<std::uint64_t, std::weak_ptr<FunctionRecord>> entries;
    std::uint64_t                                                  nextId = 1;
};

// --- JavaScript functions called by C++ ------------------------------------------------------------------------

/// The calls C++ made to one JavaScript function, and those of its typed calls that failed.
struct JsCallCounter {
    std::uint64_t              id = 0;
    std::string                label;
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> failures{0};

    void called() noexcept {
        calls.fetch_add(1, std::memory_order_relaxed);
    }
    void failed() noexcept {
        failures.fetch_add(1, std::memory_order_relaxed);
    }
};

/// A JavaScript function as webfront.inspector.jsFunctions describes it: (identifier, name, calls, failures of the typed calls).
using JsFunctionRow = std::tuple<double, std::string, double, double>;
/// A page of JavaScript functions: (rows, next).
using JsFunctionPage = std::tuple<std::vector<JsFunctionRow>, double>;

/**
 * @brief The JavaScript functions that C++ called (jsFunction, typed or not, jsFunctionSender, broadcast), by name. It exists
 * only once the developer environment enabled the inspectors: before, and in a distribution build, no call is counted.
 * A broadcast counts once, whatever the number of browsers it reaches. Thread-safe.
 */
class JsCallRegistry {
public:
    /// The counter of that name, created at its first call. Past maxJsFunctionNames names, the counter of all the others.
    [[nodiscard]] std::shared_ptr<JsCallCounter> counter(std::string_view name) {
        {
            std::shared_lock lock{mutex};
            if (const auto found = byName.find(name); found != byName.end())
                return found->second;
        }
        std::unique_lock lock{mutex};
        if (const auto found = byName.find(name); found != byName.end())
            return found->second;
        if (byName.size() >= maxJsFunctionNames) {
            if (!others)
                others = append("(other names)");
            return others;
        }
        auto created = append(boundedText(name));
        byName.emplace(std::string(name), created);
        return created;
    }

    /// The page that webfront.inspector.jsFunctions(after, count) answers, as FunctionRegistry::page.
    [[nodiscard]] JsFunctionPage page(double after, double count, std::size_t budget) const {
        const auto                 from   = detail::pageNumber(after, "cursor");
        const auto                 wanted = std::min<std::uint64_t>(detail::pageNumber(count, "count"), maxPageCount);
        const auto                 room   = detail::rowsRoom(budget);
        std::vector<JsFunctionRow> rows;
        std::size_t                used = 0;
        std::uint64_t              last = 0;
        bool                       more = false;
        std::shared_lock           lock{mutex};
        for (std::size_t index = static_cast<std::size_t>(std::min<std::uint64_t>(from, byId.size())); index < byId.size(); ++index) {
            const auto& entry = *byId[index];
            const auto  bytes = detail::encodedTuple + 3 * detail::encodedNumber + detail::encodedText(entry.label.size());
            if (rows.size() == wanted || used + bytes > room) {
                more = true;
                break;
            }
            used += bytes;
            last  = entry.id;
            rows.emplace_back(static_cast<double>(entry.id),
                              entry.label,
                              static_cast<double>(entry.calls.load(std::memory_order_relaxed)),
                              static_cast<double>(entry.failures.load(std::memory_order_relaxed)));
        }
        return {std::move(rows), more && last != 0 ? static_cast<double>(last) : 0.0};
    }

    [[nodiscard]] std::size_t size() const {
        std::shared_lock lock{mutex};
        return byId.size();
    }

private:
    std::shared_ptr<JsCallCounter> append(std::string label) {
        auto created   = std::make_shared<JsCallCounter>();
        created->id    = byId.size() + 1;
        created->label = std::move(label);
        byId.push_back(created);
        return created;
    }

    mutable std::shared_mutex                                          mutex;
    std::map<std::string, std::shared_ptr<JsCallCounter>, std::less<>> byName;
    std::vector<std::shared_ptr<JsCallCounter>>                        byId;  // the identifier is the rank, from 1
    std::shared_ptr<JsCallCounter>                                     others;
};

// --- Browsers --------------------------------------------------------------------------------------------------

/// A connected browser, as the application knows it at one moment.
struct BrowserInfo {
    std::uint64_t id           = 0;
    bool          embeddedView = false;  // it proved that it is the view of an embedded frontend
    bool          localClient  = false;  // UI::isLocalClient
    bool          started      = false;  // its onUIStarted handlers have returned
    bool          closed       = false;  // its connection closed, and is about to be released
    bool          caller       = false;  // it is the page that asks
    std::string   path;                  // UI::path
    std::string   space;                 // the path of its space, empty outside the spaces
    std::size_t   pendingWrites  = 0;    // frames queued for it and not yet written to the network
    std::size_t   pendingResults = 0;    // typed calls of C++ whose result it has not returned
    std::size_t   pendingCalls   = 0;    // its calls that a C++ callback has not answered yet
};

/// The bits of a browser's flags, in a row of webfront.inspector.browsers.
namespace browser_flag {
inline constexpr unsigned embeddedView = 1;
inline constexpr unsigned localClient  = 2;
inline constexpr unsigned started      = 4;
inline constexpr unsigned closed       = 8;
inline constexpr unsigned caller       = 16;
}  // namespace browser_flag

/// A browser as webfront.inspector.browsers describes it: (identifier, flags, path, space, pending writes, pending
/// results of typed C++ calls, pending answers of C++ callbacks).
using BrowserRow = std::tuple<double, double, std::string, std::string, double, double, double>;
/// A page of browsers: (rows, next, tasks of the server's scope). `next` is the first identifier of the following page.
using BrowserPage = std::tuple<std::vector<BrowserRow>, double, double>;

inline constexpr std::size_t maxBrowserRowBytes = detail::encodedTuple + 5 * detail::encodedNumber + 2 * detail::encodedText(maxTextBytes);
static_assert(maxBrowserRowBytes + responseHeaderBytes <= minPageBytes);

/**
 * The page that webfront.inspector.browsers(from, count) answers, from the browsers in the order of their identifiers:
 * those whose identifier is at least `from` (a connection's identifier may be 0, so the cursor is the first one to serve),
 * at most `count` and as many as fit `budget`. `next` is 0 at the end.
 */
[[nodiscard]] inline BrowserPage browserPage(const std::vector<BrowserInfo>& browsers, double from, double count, std::size_t tasks, std::size_t budget) {
    const auto              first  = detail::pageNumber(from, "cursor");
    const auto              wanted = std::min<std::uint64_t>(detail::pageNumber(count, "count"), maxPageCount);
    const auto              room   = detail::rowsRoom(budget);
    std::vector<BrowserRow> rows;
    std::size_t             used = 0;
    double                  next = 0;
    for (const auto& browser : browsers) {
        if (browser.id < first)
            continue;
        const auto path  = boundedText(browser.path);
        const auto space = boundedText(browser.space);
        const auto bytes = detail::encodedTuple + 5 * detail::encodedNumber + detail::encodedText(path.size()) + detail::encodedText(space.size());
        if (rows.size() == wanted || used + bytes > room) {
            next = static_cast<double>(browser.id);
            break;
        }
        used                += bytes;
        const unsigned flags = (browser.embeddedView ? browser_flag::embeddedView : 0u) | (browser.localClient ? browser_flag::localClient : 0u)
                               | (browser.started ? browser_flag::started : 0u) | (browser.closed ? browser_flag::closed : 0u)
                               | (browser.caller ? browser_flag::caller : 0u);
        rows.emplace_back(static_cast<double>(browser.id),
                          static_cast<double>(flags),
                          path,
                          space,
                          static_cast<double>(browser.pendingWrites),
                          static_cast<double>(browser.pendingResults),
                          static_cast<double>(browser.pendingCalls));
    }
    return {std::move(rows), next, static_cast<double>(tasks)};
}

}  // namespace webfront::inspect
