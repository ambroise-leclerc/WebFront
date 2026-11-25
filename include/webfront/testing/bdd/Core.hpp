// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — testing
// Role: The minimal part of webfront::bdd every test file needs: scenarios, sections, assertions, messages and listeners.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public maintained interface (docs/BDD.md); keep its includes light, heavier features go to their own header

// The core of webfront::bdd, the behaviour-driven test framework (docs/BDD.md).
//
// A scenario is a named object that registers itself; its sections are lambdas:
// @code
// const Scenario jsFunction{"JsFunction", [] {
//     WebFrontMock webFront;
//     given("A print JsFunction", [&] {
//         when("print is called", [&] {
//             then("the call is encoded", [&] { requireEqual(encoded.size(), 8); });
//         });
//     });
// }};
// @endcode
// As in Catch2, a scenario runs again from its start until each of its leaf sections has run once, a single new leaf
// per pass, and a section is identified by its name and by where it is written. `require` ends the scenario at the
// first failure, `check` records it and goes on.
//
// This header includes only light standard headers: it is what a test file includes. The rest is optional, each piece
// in its own header so that a file pays only for what it uses:
// - <webfront/testing/bdd/Matchers.hpp>: string matchers (contains, endsWith, equals) and check(text, matcher);
// - <webfront/testing/bdd/Approx.hpp>: approx(), floating-point comparison within a tolerance;
// - <webfront/testing/bdd/Eventually.hpp>: waits for an asynchronous state (requireEventually, requireReady...);
// - <webfront/testing/bdd/Runner.hpp>: run(), for the one file that defines main();
// - <webfront/testing/Bdd.hpp> includes them all.
// The code that runs sections and catches exceptions is compiled once, behind a pointer no optimizer follows, rather
// than once per lambda: it keeps the tests quick to compile too.

#pragma once

#include <atomic>
#include <charconv>
#include <clocale>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::bdd {

/// Thrown by a fatal failure. It does not derive from std::exception, so the `catch (const std::exception&)` of a test does
/// not swallow it.
struct AssertionFailure { };
/// Thrown by skip().
struct Skipped { };
/// The type requireThrows() accepts when it is not given one: any exception.
struct AnyException { };

/// A predicate on a string with a description of what it expects. <webfront/testing/bdd/Matchers.hpp> defines some.
template <typename M>
concept Matcher = requires(const M& matcher, std::string_view text) {
    { matcher(text) } -> std::convertible_to<bool>;
    { matcher.describe() } -> std::convertible_to<std::string>;
};

/// The customization point that writes a value of a project's type in messages and in the operands of a failed
/// checkEqual(): specialize it with a static function `void append(std::string& text, const T& value)`.
/// @code
/// template <> struct webfront::bdd::Printer<Color> {
///     static void append(std::string& text, Color color) { text += color == Color::red ? "red" : "blue"; }
/// };
/// @endcode
/// A specialization takes precedence over the built-in forms: strings, characters, booleans, numbers, enumerations
/// (written as their value) and ranges of these (written as {a, b, c}).
template <typename T>
struct Printer;

namespace impl {

/// A number written by snprintf, with '.' as its decimal separator whatever the C locale a test may have changed.
inline std::string withPoint(std::string number) {
    std::string_view point = std::localeconv()->decimal_point;
    if (auto position = number.find(point); !point.empty() && point != "." && position != std::string::npos)
        number.replace(position, point.size(), ".");
    return number;
}

/// A duration in seconds as JUnit readers expect it: a decimal number, never in scientific notation.
inline std::string seconds(double value) {
    char buffer[64];
    auto length = std::snprintf(buffer, sizeof buffer, "%.6f", value);
    return withPoint({buffer, length > 0 ? static_cast<std::size_t>(length) : 0});
}

template <typename T>
concept Customized = requires(std::string& text, const T& value) { Printer<T>::append(text, value); };

/// A range whose elements are not characters, iterated without <ranges>.
template <typename T>
concept Sequence = !std::is_convertible_v<const T&, std::string_view> && requires(const T& range) {
    range.begin() != range.end();
    *range.begin();
};

/// Whether append() writes a T. A concept cannot name itself, so the elements of a range are looked at by a variable
/// template; a range whose elements are of its own type (std::filesystem::path) is not written as a range.
template <typename T>
inline constexpr bool printable = [] {
    if constexpr (Customized<T> || std::is_convertible_v<const T&, std::string_view> || std::is_arithmetic_v<T> || std::is_enum_v<T>)
        return true;
    else if constexpr (Sequence<T>) {
        using Element = std::remove_cvref_t<decltype(*std::declval<const T&>().begin())>;
        if constexpr (std::is_same_v<Element, T>)
            return false;
        else
            return printable<Element>;
    } else
        return false;
}();

template <typename T>
concept Printable = printable<T>;

/// Whether a value is a null C string, which std::string_view cannot take.
template <typename T>
bool nullString(const T& value) {
    if constexpr (std::is_pointer_v<T>)
        return value == nullptr;
    else
        return false;
}

/// Appends the text of a value: what Printer<T> writes, a string (nullptr for a null C string), a character, a boolean,
/// a number, an enumeration or a range of these.
template <typename T>
void append(std::string& text, const T& value) {
    if constexpr (Customized<T>)
        Printer<T>::append(text, value);
    else if constexpr (std::is_convertible_v<const T&, std::string_view>)
        text += nullString(value) ? std::string_view{"nullptr"} : std::string_view{value};
    else if constexpr (std::is_same_v<T, bool>)
        text += value ? "true" : "false";
    else if constexpr (std::is_same_v<T, char>)
        text += value;
    else if constexpr (std::is_enum_v<T>)
        append(text, static_cast<std::underlying_type_t<T>>(value));
    else if constexpr (std::is_floating_point_v<T>) {
        char buffer[64];
        auto length = std::snprintf(buffer, sizeof buffer, "%.17g", static_cast<double>(value));
        text       += withPoint({buffer, length > 0 ? static_cast<std::size_t>(length) : 0});
    } else if constexpr (std::is_integral_v<T>) {
        char buffer[64];
        auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
        text.append(buffer, error == std::errc{} ? end : buffer);
    } else if constexpr (Sequence<T> && Printable<T>) {
        text      += '{';
        bool first = true;
        for (const auto& element : value) {
            if (!first)
                text += ", ";
            first = false;
            append(text, element);
        }
        text += '}';
    } else
        static_assert(Printable<T>,
                      "a message accepts strings, characters, booleans, numbers, enumerations, ranges of these and the types "
                      "webfront::bdd::Printer is specialized for");
}

template <typename... Args>
std::string concat(const Args&... args) {
    std::string text;
    (append(text, args), ...);
    return text;
}

/// An operand of a failed comparison: its text, or {?} for a type that cannot be written.
template <typename T>
void appendOperand(std::string& text, const T& value) {
    if constexpr (Printable<T>)
        append(text, value);
    else
        text += "{?}";
}

inline void print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

/// One section of a scenario, and whether every branch under it has run. A section is identified, as in Catch2, by
/// its name and by where it is written: two sibling sections of the same name are two sections. The node owns its
/// children; their addresses stay stable while the vector grows.
struct Node {
    std::string         name;
    const void*         site = nullptr;
    std::uint_least32_t line = 0;
    std::vector<Node*>  children;
    std::uint64_t       enteredChildPass = 0;
    bool                complete         = false;

    Node()                       = default;
    Node(const Node&)            = delete;
    Node& operator=(const Node&) = delete;
    ~Node() {
        for (auto* node : children)
            delete node;
    }

    Node* child(std::string_view childName, const void* childSite, std::uint_least32_t childLine) {
        for (auto* node : children)
            if (node->line == childLine && node->site == childSite && node->name == childName)
                return node;
        if (children.size() == children.capacity())
            children.reserve(children.empty() ? 4 : children.size() * 2);
        auto* node = new Node;
        children.push_back(node);
        node->name = childName;
        node->site = childSite;
        node->line = childLine;
        return node;
    }
    bool childrenComplete() const {
        for (const auto* node : children)
            if (!node->complete)
                return false;
        return true;
    }
};

struct Entry {
    std::string              title;
    std::vector<std::string> tags;
    void (*body)();
    std::string_view    file;
    std::uint_least32_t line;
};

struct Listeners {
    std::vector<void (*)()> runStarting;
    std::vector<void (*)()> scenarioStarting;
    std::vector<void (*)()> runEnding;
};

/// The state of the scenario being run.
struct State {
    Node*                                                      current   = nullptr;
    std::uint64_t                                              pass      = 0;
    bool                                                       unwinding = false;
    std::vector<std::pair<std::string_view, std::string_view>> path;  ///< The kind and the name of the sections entered.
    std::string_view                                           title;
    std::atomic<std::uint64_t>                                 assertions{0};
    std::atomic<std::uint64_t>                                 failures{0};
    std::string                                                report;  ///< The failures of the scenario, for JUnit.
    std::atomic_flag                                           reporting;
};

inline std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}
inline Listeners& listeners() {
    static Listeners all;
    return all;
}
inline State& state() {
    static State current;
    return current;
}
/// The messages of the info() objects alive on this thread.
inline std::vector<std::string>& infos() {
    thread_local std::vector<std::string> messages;
    return messages;
}

/// The lines of a source file, read with <cstdio> rather than <fstream>, which would weigh on every test file.
inline std::vector<std::string> readLines(const char* name) {
    std::vector<std::string> lines;
    std::FILE*               file = nullptr;
#ifdef _WIN32
    // MSVC deprecates fopen.
    if (fopen_s(&file, name, "rb") != 0)
        file = nullptr;
#else
    file = std::fopen(name, "rb");
#endif
    if (!file)
        return lines;
    std::string content;
    char        buffer[4096];
    for (std::size_t read; (read = std::fread(buffer, 1, sizeof buffer, file)) != 0;)
        content.append(buffer, read);
    std::fclose(file);
    for (std::size_t start = 0; start < content.size();) {
        auto end = content.find('\n', start);
        if (end == std::string::npos)
            end = content.size();
        std::string line = content.substr(start, end - start);
        if (line.ends_with('\r'))
            line.pop_back();
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

/// The text of a line of a source file, read once per file and only to report a failure.
inline std::string sourceLine(const std::source_location& location) {
    static std::vector<std::pair<std::string, std::vector<std::string>>> files;
    std::vector<std::string>*                                            lines = nullptr;
    for (auto& [name, content] : files)
        if (name == location.file_name())
            lines = &content;
    if (!lines)
        lines = &files.emplace_back(location.file_name(), readLines(location.file_name())).second;
    if (location.line() == 0 || location.line() > lines->size())
        return {};
    std::string_view line  = (*lines)[location.line() - 1];
    auto             first = line.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string{} : std::string{line.substr(first)};
}

/// Holds the lock of the reports, which also guards the path of sections a failure on another thread reads.
class Lock {
public:
    explicit Lock(std::atomic_flag& flag) : held{flag} {
        while (held.test_and_set(std::memory_order_acquire))
            held.wait(true, std::memory_order_relaxed);
    }
    Lock(const Lock&)            = delete;
    Lock& operator=(const Lock&) = delete;
    ~Lock() {
        held.clear(std::memory_order_release);
        held.notify_one();
    }

private:
    std::atomic_flag& held;
};

/// Reports a failure with its location, its source line, the sections that lead to it and the messages in scope.
/// Several threads may fail at once: the lock covers the whole report, the cache of the source lines included.
inline void reportFailure(const std::source_location& location, std::string_view what) {
    auto&       current = state();
    Lock        lock{current.reporting};
    std::string text;
    if (location.line() != 0)
        text += concat(location.file_name(), ':', location.line(), ": ");
    text += "FAILED";
    if (auto line = sourceLine(location); !line.empty())
        text += concat("\n    ", line);
    if (!what.empty())
        text += concat("\n  ", what);
    text += concat("\n  in Scenario: ", current.title);
    for (const auto& [kind, name] : current.path)
        text += concat("\n     ", kind, name);
    for (const auto& message : infos())
        text += concat("\n  with: ", message);
    text += '\n';
    current.failures.fetch_add(1, std::memory_order_relaxed);
    current.report += text;
    print(text);
}

inline void count() {
    state().assertions.fetch_add(1, std::memory_order_relaxed);
}

inline std::string exceptionMessage(std::exception_ptr exception) {
    try {
        std::rethrow_exception(std::move(exception));
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "an exception that is not a std::exception";
    }
}

template <typename F>
void invoke(void* target) {
    (*static_cast<F*>(target))();
}

/// A reference to a callable that is not copied, made implicitly from a lambda: the functions that take one are not
/// templates, so that a section or an operation costs the compiler one small instantiation, not a family of them.
struct Body {
    void* object        = nullptr;
    void (*call)(void*) = nullptr;

    Body() = default;
    template <typename F>
        requires(std::invocable<F&> && !std::same_as<std::remove_cvref_t<F>, Body>)
    Body(F&& callable) : object{const_cast<void*>(static_cast<const void*>(__builtin_addressof(callable)))}, call{&invoke<std::remove_reference_t<F>>} { }
    void operator()() const {
        call(object);
    }
};

/// Runs a section if this pass may enter it: a section is entered when it still has a branch to run and no sibling was
/// entered during this pass.
inline void runSection(std::string_view kind, std::string_view name, const void* site, std::uint_least32_t line, Body body) {
    auto& current = state();
    Node* parent  = current.current;
    Node* node    = parent->child(name, site, line);
    if (node->complete || parent->enteredChildPass == current.pass)
        return;
    parent->enteredChildPass = current.pass;
    current.current          = node;
    {
        Lock lock{current.reporting};
        current.path.emplace_back(kind, node->name);
    }
    struct Restore {
        State& owner;
        Node*  previous;
        ~Restore() {
            owner.current = previous;
            Lock lock{owner.reporting};
            owner.path.pop_back();
        }
    } restore{current, parent};
    try {
        body();
    } catch (const AssertionFailure&) {
        // The section that failed is done; its ancestors run again, to find the sections the failure kept hidden.
        node->complete    = !current.unwinding;
        current.unwinding = true;
        throw;
    } catch (const Skipped&) {
        node->complete    = !current.unwinding;
        current.unwinding = true;
        throw;
    } catch (...) {
        node->complete    = true;
        current.unwinding = true;
        reportFailure(std::source_location{}, concat("unexpected exception: ", exceptionMessage(std::current_exception())));
        throw AssertionFailure{};
    }
    node->complete = node->childrenComplete();
}

/// Called through a volatile pointer, which no optimizer follows: inlining every section, and the lambdas it calls,
/// into its scenario makes functions so large that an optimized build takes minutes per file.
inline void (*volatile sectionRunner)(std::string_view, std::string_view, const void*, std::uint_least32_t, Body) = runSection;

inline void section(std::string_view kind, std::string_view name, Body body, const std::source_location& location) {
    sectionRunner(kind, name, location.file_name(), location.line(), body);
}

/// The first part of a section name made of several parts, which records where the section is written.
struct NamePart {
    std::string_view     text;
    std::source_location location;
    template <typename T>
        requires std::convertible_to<const T&, std::string_view>
    NamePart(const T& part, std::source_location where = std::source_location::current()) : text{part}, location{where} { }
};

/// A section whose name is made of several parts: given("q=", value, [&] { ... }).
template <typename... Args>
void composedSection(std::string_view kind, const NamePart& first, Args&... arguments) {
    std::string name{first.text};
    Body        body{};
    auto        take = [&]<typename T>(T& argument) {
        if constexpr (std::invocable<T&>)
            body = Body{argument};
        else
            append(name, argument);
    };
    (take(arguments), ...);
    sectionRunner(kind, name, first.location.file_name(), first.location.line(), body);
}

/// What a condition may be: anything convertible to bool except a string, which would always be true.
template <typename T>
concept Testable = !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<std::decay_t<T>>>, char> && requires(const T& value) { static_cast<bool>(value); };

/// A condition that explains its own failure, such as the result of a comparison with approx().
template <typename T>
concept Explained = requires(const T& condition) {
    { condition.explain() } -> std::convertible_to<std::string>;
};

/// Whether the last argument is the body of a section whose name has several parts.
template <typename... Args>
concept ComposedSection = sizeof...(Args) >= 2 && std::invocable<std::remove_reference_t<decltype((std::declval<Args&>(), ...))>&>;

/// An integer that std::cmp_equal accepts: not a boolean nor a character type.
template <typename T>
concept PlainInteger = std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool> && !std::is_same_v<std::remove_cv_t<T>, char>
                       && !std::is_same_v<std::remove_cv_t<T>, wchar_t> && !std::is_same_v<std::remove_cv_t<T>, char8_t>
                       && !std::is_same_v<std::remove_cv_t<T>, char16_t> && !std::is_same_v<std::remove_cv_t<T>, char32_t>;

/// Whether two values are equal: two integers are compared by value whatever their signedness, so that
/// checkEqual(items.size(), 3) neither warns nor converts -1 to a large unsigned number, and two strings by their
/// characters, so that two `const char*` are not compared as pointers. A null C string equals only another one.
template <typename L, typename R>
bool equal(const L& left, const R& right) {
    if constexpr (PlainInteger<L> && PlainInteger<R>)
        return std::cmp_equal(left, right);
    else if constexpr (std::is_convertible_v<const L&, std::string_view> && std::is_convertible_v<const R&, std::string_view>) {
        if (nullString(left) || nullString(right))
            return nullString(left) && nullString(right);
        return std::string_view{left} == std::string_view{right};
    } else
        return static_cast<bool>(left == right);
}

/// The report of a failed comparison: both operands, each with its own label.
template <typename L, typename R>
std::string operands(const L& left, const R& right) {
    std::string text{"left:  "};
    appendOperand(text, left);
    text += "\n  right: ";
    appendOperand(text, right);
    return text;
}

}  // namespace impl

/// Registers a scenario. Its title names it in the reports and in CTest, prefixed with "Scenario: ". The body is a
/// lambda without captures.
class Scenario {
public:
    template <std::convertible_to<void (*)()> F>
    Scenario(std::string_view title, F body, std::source_location location = std::source_location::current()) : Scenario(title, {}, body, location) { }

    template <std::convertible_to<void (*)()> F>
    Scenario(std::string_view title, std::initializer_list<std::string_view> tags, F body, std::source_location location = std::source_location::current()) {
        impl::registry().push_back(
            {std::string{title}, std::vector<std::string>(tags.begin(), tags.end()), static_cast<void (*)()>(body), location.file_name(), location.line()});
    }
};

/// Registers functions, lambdas without captures or null, called before the first scenario, before each scenario and
/// after the last one. `runEnding` is called once the selected scenarios have run, whatever their outcome, before the
/// summary and the JUnit report, and only when `runStarting` was called: a run that selects nothing calls neither. An
/// exception it throws is reported and fails the run.
class Listener {
public:
    Listener(void (*runStarting)(), void (*scenarioStarting)(), void (*runEnding)() = nullptr) {
        if (runStarting)
            impl::listeners().runStarting.push_back(runStarting);
        if (scenarioStarting)
            impl::listeners().scenarioStarting.push_back(scenarioStarting);
        if (runEnding)
            impl::listeners().runEnding.push_back(runEnding);
    }
};

inline void given(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("Given: ", name, body, location);
}
inline void andGiven(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("And given: ", name, body, location);
}
inline void when(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("When: ", name, body, location);
}
inline void andWhen(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("And when: ", name, body, location);
}
inline void then(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("Then: ", name, body, location);
}
inline void andThen(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("And: ", name, body, location);
}
inline void section(std::string_view name, impl::Body body, std::source_location location = std::source_location::current()) {
    impl::section("", name, body, location);
}

/// Sections whose name is made of several parts, the body coming last: when("the value ", value, [&] { ... }).
template <typename... Args>
    requires impl::ComposedSection<Args...>
void given(impl::NamePart first, Args&&... arguments) {
    impl::composedSection("Given: ", first, arguments...);
}
template <typename... Args>
    requires impl::ComposedSection<Args...>
void when(impl::NamePart first, Args&&... arguments) {
    impl::composedSection("When: ", first, arguments...);
}
template <typename... Args>
    requires impl::ComposedSection<Args...>
void then(impl::NamePart first, Args&&... arguments) {
    impl::composedSection("Then: ", first, arguments...);
}
template <typename... Args>
    requires impl::ComposedSection<Args...>
void section(impl::NamePart first, Args&&... arguments) {
    impl::composedSection("", first, arguments...);
}

/// Adds a message to the failures reported while the returned object lives.
class [[nodiscard]] Info {
public:
    explicit Info(std::string message) {
        impl::infos().push_back(std::move(message));
    }
    Info(const Info&)            = delete;
    Info& operator=(const Info&) = delete;
    ~Info() {
        impl::infos().pop_back();
    }
};
template <typename... Args>
Info info(const Args&... args) {
    return Info{impl::concat(args...)};
}

/// Checks a condition and records a failure if it is false. A condition whose type has `std::string explain() const`
/// (the result of a comparison with approx(), for one) adds that text to the failure.
template <impl::Testable T>
bool check(const T& condition, std::source_location location = std::source_location::current()) {
    impl::count();
    if (static_cast<bool>(condition)) [[likely]]
        return true;
    if constexpr (impl::Explained<T>)
        impl::reportFailure(location, std::string{condition.explain()});
    else
        impl::reportFailure(location, {});
    return false;
}
/// Checks a condition and ends the scenario if it is false.
template <impl::Testable T>
void require(const T& condition, std::source_location location = std::source_location::current()) {
    if (!check(condition, location)) [[unlikely]]
        throw AssertionFailure{};
}

/// Checks that two values are equal and, if they are not, reports both: what Printer<T> or the built-in forms write, or
/// {?} for a type neither can write. Two integers are compared by value whatever their signedness, two strings by their
/// characters; any other pair with `left == right`.
template <typename L, typename R>
bool checkEqual(const L& left, const R& right, std::source_location location = std::source_location::current()) {
    impl::count();
    if (impl::equal(left, right)) [[likely]]
        return true;
    impl::reportFailure(location, impl::operands(left, right));
    return false;
}
/// Checks that two values are equal like checkEqual() and ends the scenario if they are not.
template <typename L, typename R>
void requireEqual(const L& left, const R& right, std::source_location location = std::source_location::current()) {
    if (!checkEqual(left, right, location)) [[unlikely]]
        throw AssertionFailure{};
}

namespace impl {

/// Runs an operation and returns what it threw, if anything. A failure or a skip of an assertion inside it goes on.
inline std::exception_ptr runCaught(Body operation) {
    try {
        operation();
        return nullptr;
    } catch (const AssertionFailure&) {
        throw;
    } catch (const Skipped&) {
        throw;
    } catch (...) {
        return std::current_exception();
    }
}
/// Called through a volatile pointer for the reason sectionRunner is.
inline std::exception_ptr (*volatile caughtRunner)(Body) = runCaught;

/// A matcher seen through its type: empty when an exception is not matched.
struct Predicate {
    const void* matcher                         = nullptr;
    bool (*test)(const void*, std::string_view) = nullptr;
    std::string (*describe)(const void*)        = nullptr;

    template <Matcher M>
    static Predicate of(const M& matcher) {
        return {&matcher,
                [](const void* target, std::string_view text) {
                    return static_cast<bool>((*static_cast<const M*>(target))(text));
                },
                [](const void* target) {
                    return std::string{static_cast<const M*>(target)->describe()};
                }};
    }
};

/// Whether an exception is an E whose message matches.
template <typename E>
bool thrown(std::exception_ptr exception, Predicate matcher, const std::source_location& location) {
    count();
    if (!exception) {
        reportFailure(location, "no exception was thrown");
        return false;
    }
    std::string message;
    if constexpr (std::is_same_v<E, AnyException>)
        message = exceptionMessage(exception);
    else {
        try {
            std::rethrow_exception(exception);
        } catch (const E& error) {
            if constexpr (requires { error.what(); })
                message = error.what();
        } catch (...) {
            reportFailure(location, concat("an exception of another type was thrown: ", exceptionMessage(exception)));
            return false;
        }
    }
    if (!matcher.test || matcher.test(matcher.matcher, message))
        return true;
    reportFailure(location, concat("the exception \"", message, "\" does not match: ", matcher.describe(matcher.matcher)));
    return false;
}

}  // namespace impl

/// Checks that an operation throws an E (any exception by default), whose message matches if a matcher is given.
template <typename E = AnyException>
bool checkThrows(impl::Body operation, std::source_location location = std::source_location::current()) {
    return impl::thrown<E>(impl::caughtRunner(operation), {}, location);
}
template <typename E = AnyException, Matcher M>
bool checkThrows(impl::Body operation, const M& matcher, std::source_location location = std::source_location::current()) {
    return impl::thrown<E>(impl::caughtRunner(operation), impl::Predicate::of(matcher), location);
}
template <typename E = AnyException>
void requireThrows(impl::Body operation, std::source_location location = std::source_location::current()) {
    if (!checkThrows<E>(operation, location)) [[unlikely]]
        throw AssertionFailure{};
}
template <typename E = AnyException, Matcher M>
void requireThrows(impl::Body operation, const M& matcher, std::source_location location = std::source_location::current()) {
    if (!checkThrows<E>(operation, matcher, location)) [[unlikely]]
        throw AssertionFailure{};
}

/// Checks that an operation does not throw.
inline bool checkNothrow(impl::Body operation, std::source_location location = std::source_location::current()) {
    impl::count();
    if (auto exception = impl::caughtRunner(operation); exception) [[unlikely]] {
        impl::reportFailure(location, impl::concat("unexpected exception: ", impl::exceptionMessage(exception)));
        return false;
    }
    return true;
}
inline void requireNothrow(impl::Body operation, std::source_location location = std::source_location::current()) {
    if (!checkNothrow(operation, location)) [[unlikely]]
        throw AssertionFailure{};
}

/// A message and the location it comes from: the functions that take a variable message cannot default a location.
struct Located {
    std::string_view     text;
    std::source_location location;
    template <typename T>
        requires std::convertible_to<const T&, std::string_view>
    Located(const T& message, std::source_location where = std::source_location::current()) : text{message}, location{where} { }
};

/// Records a failure and goes on; the message is the concatenation of the arguments.
template <typename... Args>
void failCheck(Located message, const Args&... more) {
    impl::count();
    impl::reportFailure(message.location, impl::concat(message.text, more...));
}
/// Records a failure and ends the scenario.
template <typename... Args>
[[noreturn]] void fail(Located message, const Args&... more) {
    failCheck(message, more...);
    throw AssertionFailure{};
}
/// Ends the scenario, which is reported as skipped unless it has already failed.
template <typename... Args>
[[noreturn]] void skip(Located message, const Args&... more) {
    impl::print(impl::concat(message.location.file_name(), ':', message.location.line(), ": SKIPPED: ", message.text, more..., '\n'));
    throw Skipped{};
}
/// Prints a warning, which does not fail the scenario.
template <typename... Args>
void warn(Located message, const Args&... more) {
    impl::print(impl::concat(message.location.file_name(), ':', message.location.line(), ": warning: ", message.text, more..., '\n'));
}
/// Records a successful assertion.
inline void succeed(Located) {
    impl::count();
}

}  // namespace webfront::bdd
