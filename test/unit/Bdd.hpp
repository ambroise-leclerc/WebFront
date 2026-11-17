// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @file
/// @brief A minimal behaviour-driven test framework in a single header.
///
/// A scenario is a named object that registers itself; its sections are lambdas:
/// @code
/// const Scenario jsFunction{"JsFunction", [] {
///     WebFrontMock webFront;
///     given("A print JsFunction", [&] {
///         when("print is called", [&] {
///             then("the call is encoded", [&] { require(encoded.size() == 8); });
///         });
///     });
/// }};
/// @endcode
/// As in Catch2, a scenario runs again from its start until each of its leaf sections has run once, a single new leaf
/// per pass, and a section is identified by its name and by where it is written. `require` ends the scenario at the
/// first failure, `check` records it and goes on.
///
/// The header includes only light standard headers, and the code that runs sections and catches exceptions is compiled
/// once, behind a pointer no optimizer follows, rather than once per lambda: both keep the tests quick to compile.
#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <clocale>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
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

/// A predicate on a string with a description of what it expects.
template <typename M>
concept Matcher = requires(const M& matcher, std::string_view text) {
    { matcher(text) } -> std::convertible_to<bool>;
    { matcher.describe() } -> std::convertible_to<std::string>;
};

/// Matches a string that contains a text.
struct Contains {
    std::string text;
    bool        operator()(std::string_view value) const {
        return value.find(text) != std::string_view::npos;
    }
    std::string describe() const {
        return "contains \"" + text + '"';
    }
};
/// Matches a string that ends with a text.
struct EndsWith {
    std::string text;
    bool        operator()(std::string_view value) const {
        return value.ends_with(text);
    }
    std::string describe() const {
        return "ends with \"" + text + '"';
    }
};
/// Matches a string equal to a text.
struct Equals {
    std::string text;
    bool        operator()(std::string_view value) const {
        return value == text;
    }
    std::string describe() const {
        return "equals \"" + text + '"';
    }
};
/// Matches what another matcher does not.
template <Matcher M>
struct Not {
    M    matcher;
    bool operator()(std::string_view value) const {
        return !matcher(value);
    }
    std::string describe() const {
        return "not " + matcher.describe();
    }
};
template <Matcher M>
Not<M> operator!(M matcher) {
    return {std::move(matcher)};
}
inline Contains contains(std::string text) {
    return {std::move(text)};
}
inline EndsWith endsWith(std::string text) {
    return {std::move(text)};
}
inline Equals equals(std::string text) {
    return {std::move(text)};
}

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

/// Appends the text of a value: a string, a character, a boolean, a number or an enumeration.
template <typename T>
void append(std::string& text, const T& value) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>)
        text += std::string_view{value};
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
    } else
        static_assert(std::is_arithmetic_v<T>, "a message accepts strings, characters, booleans, numbers and enumerations");
}

template <typename... Args>
std::string concat(const Args&... args) {
    std::string text;
    (append(text, args), ...);
    return text;
}

inline void print(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

/// One section of a scenario, and whether every branch under it has run. A section is identified, as in Catch2, by
/// its name and by where it is written: two sibling sections of the same name are two sections.
struct Node {
    std::string                        name;
    const void*                        site = nullptr;
    std::uint_least32_t                line = 0;
    std::vector<std::unique_ptr<Node>> children;
    std::uint64_t                      enteredChildPass = 0;
    bool                               complete         = false;

    Node* child(std::string_view childName, const void* childSite, std::uint_least32_t childLine) {
        for (auto& node : children)
            if (node->line == childLine && node->site == childSite && node->name == childName)
                return node.get();
        children.push_back(std::make_unique<Node>());
        auto& node = *children.back();
        node.name  = childName;
        node.site  = childSite;
        node.line  = childLine;
        return &node;
    }
    bool childrenComplete() const {
        for (const auto& node : children)
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

/// The text of a line of a source file, read once per file and only to report a failure.
inline std::string sourceLine(const std::source_location& location) {
    static std::vector<std::pair<std::string, std::vector<std::string>>> files;
    std::vector<std::string>*                                            lines = nullptr;
    for (auto& [name, content] : files)
        if (name == location.file_name())
            lines = &content;
    if (!lines) {
        lines = &files.emplace_back(location.file_name(), std::vector<std::string>{}).second;
        std::ifstream file{location.file_name(), std::ios::binary};
        for (std::string line; std::getline(file, line);) {
            if (line.ends_with('\r'))
                line.pop_back();
            lines->push_back(std::move(line));
        }
    }
    if (location.line() == 0 || location.line() > lines->size())
        return {};
    std::string_view line  = (*lines)[location.line() - 1];
    auto             first = line.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string{} : std::string{line.substr(first)};
}

/// Reports a failure with its location, its source line, the sections that lead to it and the messages in scope.
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
    Body(F&& callable) : object{const_cast<void*>(static_cast<const void*>(std::addressof(callable)))}, call{&invoke<std::remove_reference_t<F>>} { }
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

/// Whether the last argument is the body of a section whose name has several parts.
template <typename... Args>
concept ComposedSection = sizeof...(Args) >= 2 && std::invocable<std::remove_reference_t<decltype((std::declval<Args&>(), ...))>&>;

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

/// Registers functions, lambdas without captures, called before the first scenario and before each scenario.
class Listener {
public:
    Listener(void (*runStarting)(), void (*scenarioStarting)()) {
        if (runStarting)
            impl::listeners().runStarting.push_back(runStarting);
        if (scenarioStarting)
            impl::listeners().scenarioStarting.push_back(scenarioStarting);
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

/// Checks a condition and records a failure if it is false.
template <impl::Testable T>
bool check(const T& condition, std::source_location location = std::source_location::current()) {
    impl::count();
    if (static_cast<bool>(condition)) [[likely]]
        return true;
    impl::reportFailure(location, {});
    return false;
}
/// Checks a condition and ends the scenario if it is false.
template <impl::Testable T>
void require(const T& condition, std::source_location location = std::source_location::current()) {
    if (!check(condition, location)) [[unlikely]]
        throw AssertionFailure{};
}

/// Checks a string against a matcher.
template <typename T, Matcher M>
    requires std::convertible_to<const T&, std::string_view>
bool check(const T& value, const M& matcher, std::source_location location = std::source_location::current()) {
    impl::count();
    std::string_view text{value};
    if (matcher(text)) [[likely]]
        return true;
    impl::reportFailure(location, impl::concat('"', text, "\" ", matcher.describe()));
    return false;
}
template <typename T, Matcher M>
    requires std::convertible_to<const T&, std::string_view>
void require(const T& value, const M& matcher, std::source_location location = std::source_location::current()) {
    if (!check(value, matcher, location)) [[unlikely]]
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

namespace impl {

/// A condition seen through a pointer, like Body. It points to a callable of the caller that must outlive it: the public
/// functions build it from a lambda of their own, which also accepts free functions.
struct Condition {
    void* object        = nullptr;
    bool (*call)(void*) = nullptr;

    template <typename F>
    explicit Condition(F& predicate)
        : object{std::addressof(predicate)}, call{[](void* target) {
              return static_cast<bool>((*static_cast<F*>(target))());
          }} { }
    bool operator()() const {
        return call(object);
    }
};

/// The delay of the waiting functions when none is given.
inline constexpr std::chrono::milliseconds defaultTimeout{30000};

/// The factor of WEBFRONT_TEST_TIMEOUT_FACTOR: a slow job (sanitizers, emulation) lengthens every wait. It is a decimal
/// number such as 2 or 2.5 of at most six digits before the point, which test/unit/CMakeLists.txt reads the same way to
/// size the CTest limits; anything else, or zero, is ignored.
inline double parseFactor(const char* text) {
    if (!text || !*text)
        return 1.0;
    std::string_view digits{text};
    auto             point = digits.find('.');
    auto             whole = digits.substr(0, point);
    auto             part  = point == std::string_view::npos ? std::string_view{} : digits.substr(point + 1);
    auto             plain = [](std::string_view run) {
        return run.find_first_not_of("0123456789") == std::string_view::npos;
    };
    if (whole.empty() || !plain(whole) || !plain(part) || (point != std::string_view::npos && part.empty()))
        return 1.0;
    // By hand, not strtod: the decimal point of the C locale a test may have changed does not matter.
    double factor = 0.0, scale = 1.0;
    for (char digit : whole)
        factor = factor * 10.0 + (digit - '0');
    for (char digit : part) {
        scale  /= 10.0;
        factor += (digit - '0') * scale;
    }
    return factor > 0.0 && factor < 1e6 ? factor : 1.0;
}
inline double timeoutFactor() {
    static const double factor = [] {
#ifdef _WIN32
        // MSVC deprecates getenv.
        char*        value  = nullptr;
        size_t       length = 0;
        const double parsed = _dupenv_s(&value, &length, "WEBFRONT_TEST_TIMEOUT_FACTOR") == 0 ? parseFactor(value) : 1.0;
        std::free(value);
        return parsed;
#else
        return parseFactor(std::getenv("WEBFRONT_TEST_TIMEOUT_FACTOR"));
#endif
    }();
    return factor;
}

/// The report of a delay that ran out: the delay actually waited for, and the one asked for when a factor changed it.
inline std::string timeoutMessage(std::chrono::milliseconds limit, double factor, double elapsed) {
    std::string text = concat("not met within ", seconds(std::chrono::duration<double>(limit).count() * factor), " s");
    if (factor != 1.0)
        text += concat(" (", seconds(std::chrono::duration<double>(limit).count()), " s x ", factor, ")");
    return text + concat(" (waited ", seconds(elapsed), " s)");
}

/// Evaluates a condition until it holds or the delay (times the factor) is over, pausing a little longer after each
/// attempt. Returns the time waited and whether it held.
inline std::pair<double, bool> pollCondition(Condition condition, std::chrono::milliseconds limit) {
    using Clock         = std::chrono::steady_clock;
    const auto start    = Clock::now();
    const auto deadline = start + std::chrono::duration_cast<Clock::duration>(limit * timeoutFactor());
    auto       pause    = std::chrono::microseconds{100};
    for (;;) {
        if (condition())
            return {std::chrono::duration<double>(Clock::now() - start).count(), true};
        if (Clock::now() >= deadline)
            return {std::chrono::duration<double>(Clock::now() - start).count(), false};
        std::this_thread::sleep_for(pause);
        pause = std::min<std::chrono::microseconds>(pause * 2, std::chrono::milliseconds{5});
    }
}
/// Called through a volatile pointer for the reason sectionRunner is.
inline std::pair<double, bool> (*volatile conditionPoller)(Condition, std::chrono::milliseconds) = pollCondition;

inline bool eventually(Condition condition, std::chrono::milliseconds limit, const std::source_location& location) {
    count();
    auto [elapsed, held] = conditionPoller(condition, limit);
    if (held) [[likely]]
        return true;
    reportFailure(location, timeoutMessage(limit, timeoutFactor(), elapsed));
    return false;
}

template <typename Future>
concept Waitable = requires(const Future& future) { future.wait_for(std::chrono::milliseconds{0}); };

/// Whether a future is ready, at once; a deferred one never becomes ready by waiting. A future without a shared state (not
/// assigned, or already consumed) is reported as `invalid`: waiting on it is undefined.
template <typename Future>
bool readyNow(const Future& future, bool& deferred, bool& invalid) {
    if constexpr (requires { future.valid(); }) {
        invalid = !future.valid();
        if (invalid)
            return false;
    }
    using Status = decltype(future.wait_for(std::chrono::milliseconds{0}));
    auto status  = future.wait_for(std::chrono::milliseconds{0});
    if constexpr (requires { Status::deferred; })
        deferred = status == Status::deferred;
    return status == Status::ready;
}

}  // namespace impl

/// Waits for an observable condition instead of sleeping a fixed time: a loaded machine only makes it take longer. The
/// condition is evaluated after pauses that grow from 0.1 to 5 ms. A failure reports the source line of the call, the
/// delay and the time waited. The delay is 30 seconds unless given, and WEBFRONT_TEST_TIMEOUT_FACTOR multiplies every
/// delay. The condition is any callable returning something testable, a free function included.
template <std::invocable P>
bool checkEventually(P&& predicate, std::chrono::milliseconds limit, std::source_location location = std::source_location::current()) {
    auto test = [&] {
        return static_cast<bool>(predicate());
    };
    return impl::eventually(impl::Condition{test}, limit, location);
}
template <std::invocable P>
bool checkEventually(P&& predicate, std::source_location location = std::source_location::current()) {
    return checkEventually(std::forward<P>(predicate), impl::defaultTimeout, location);
}
template <std::invocable P>
void requireEventually(P&& predicate, std::chrono::milliseconds limit, std::source_location location = std::source_location::current()) {
    if (!checkEventually(std::forward<P>(predicate), limit, location)) [[unlikely]]
        throw AssertionFailure{};
}
template <std::invocable P>
void requireEventually(P&& predicate, std::source_location location = std::source_location::current()) {
    requireEventually(std::forward<P>(predicate), impl::defaultTimeout, location);
}

/// Waits like checkEventually() but records nothing and returns the result: for a thread that cannot assert, or a wait
/// that precedes a cleanup, with the assertion made afterwards by the scenario.
template <std::invocable P>
bool waitUntil(P&& predicate, std::chrono::milliseconds limit = impl::defaultTimeout) {
    auto test = [&] {
        return static_cast<bool>(predicate());
    };
    return impl::conditionPoller(impl::Condition{test}, limit).second;
}

/// Waits for a std::future or std::shared_future to be ready, which does not consume it. A deferred future, which only
/// becomes ready when it is waited on, fails at once.
template <impl::Waitable Future>
bool checkReady(const Future& future, std::chrono::milliseconds limit, std::source_location location = std::source_location::current()) {
    bool deferred = false, invalid = false;
    if (!impl::readyNow(future, deferred, invalid) && (deferred || invalid)) {
        impl::count();
        impl::reportFailure(location,
                            invalid ? "the future has no shared state: it was consumed or never assigned"
                                    : "the future is deferred: it is never ready until it is waited on");
        return false;
    }
    return checkEventually(
        [&] {
            return impl::readyNow(future, deferred, invalid);
        },
        limit,
        location);
}
template <impl::Waitable Future>
bool checkReady(const Future& future, std::source_location location = std::source_location::current()) {
    return checkReady(future, impl::defaultTimeout, location);
}
template <impl::Waitable Future>
void requireReady(const Future& future, std::chrono::milliseconds limit, std::source_location location = std::source_location::current()) {
    if (!checkReady(future, limit, location)) [[unlikely]]
        throw AssertionFailure{};
}
template <impl::Waitable Future>
void requireReady(const Future& future, std::source_location location = std::source_location::current()) {
    requireReady(future, impl::defaultTimeout, location);
}
/// Waits for a future like waitUntil(), without recording anything. A deferred future or one without a shared state returns
/// false at once.
template <impl::Waitable Future>
bool waitReady(const Future& future, std::chrono::milliseconds limit = impl::defaultTimeout) {
    bool deferred = false, invalid = false;
    if (!impl::readyNow(future, deferred, invalid) && (deferred || invalid))
        return false;
    return waitUntil(
        [&] {
            return impl::readyNow(future, deferred, invalid);
        },
        limit);
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

namespace impl {

inline double now() {
    std::timespec time{};
    if (std::timespec_get(&time, TIME_UTC) != TIME_UTC)
        return 0.;
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

struct Options {
    std::vector<std::string_view> filters;
    std::string_view              exact;
    std::string_view              junit;
    bool                          list        = false;
    bool                          randomOrder = true;
    std::uint64_t                 seed        = 0;
    std::uint64_t                 shardCount  = 1;
    std::uint64_t                 shardIndex  = 0;
};

enum class Outcome { passed, failed, skipped };

struct Result {
    const Entry* entry;
    Outcome      outcome;
    double       seconds;
    std::string  report;
};

/// Whether a scenario matches a filter: "[a][b]" requires every tag, otherwise the title, with or without "Scenario: ",
/// may start or end with a '*' wildcard.
inline bool matches(const Entry& entry, std::string_view filter) {
    if (filter.starts_with('[')) {
        while (filter.starts_with('[')) {
            auto end = filter.find(']');
            if (end == std::string_view::npos)
                return false;
            auto tag   = filter.substr(1, end - 1);
            bool found = false;
            for (const auto& entryTag : entry.tags)
                found = found || entryTag == tag;
            if (!found)
                return false;
            filter.remove_prefix(end + 1);
        }
        return filter.empty();
    }
    if (filter.starts_with("Scenario: "))
        filter.remove_prefix(10);
    std::string_view title  = entry.title;
    bool             prefix = filter.ends_with('*'), suffix = filter.starts_with('*');
    if (prefix)
        filter.remove_suffix(1);
    if (suffix && !filter.empty())
        filter.remove_prefix(1);
    if (prefix && suffix)
        return title.find(filter) != std::string_view::npos;
    if (prefix)
        return title.starts_with(filter);
    if (suffix)
        return title.ends_with(filter);
    return title == filter;
}

/// Whether a scenario is selected: each argument is a comma-separated list of alternatives.
inline bool selected(const Entry& entry, const Options& options) {
    if (!options.exact.empty())
        return entry.title == options.exact;
    if (options.filters.empty())
        return true;
    for (auto rest : options.filters)
        while (true) {
            auto comma = rest.find(',');
            if (matches(entry, rest.substr(0, comma)))
                return true;
            if (comma == std::string_view::npos)
                break;
            rest.remove_prefix(comma + 1);
        }
    return false;
}

/// Runs every pass of a scenario.
inline Result runScenario(const Entry& entry) {
    auto& current = state();
    for (auto listener : listeners().scenarioStarting)
        listener();
    Node root;
    current.title = entry.title;
    current.report.clear();
    auto failuresBefore = current.failures.load();
    bool skipped        = false;
    auto start          = now();
    for (current.pass = 1;; ++current.pass) {
        current.current   = &root;
        current.unwinding = false;
        current.path.clear();
        bool aborted = false;
        try {
            entry.body();
        } catch (const AssertionFailure&) {
            aborted = !current.unwinding;
        } catch (const Skipped&) {
            skipped = true;
        } catch (...) {
            reportFailure(std::source_location{}, concat("unexpected exception: ", exceptionMessage(std::current_exception())));
            aborted = true;
        }
        root.complete = root.childrenComplete();
        // A failure outside any section, a skip, a pass that entered no section or the end of every branch ends the
        // scenario. After a failure in a section, another pass looks for the sections that follow it.
        if (aborted || skipped || root.enteredChildPass != current.pass || (root.complete && !current.unwinding))
            break;
    }
    auto outcome = current.failures.load() != failuresBefore ? Outcome::failed : skipped ? Outcome::skipped : Outcome::passed;
    return {&entry, outcome, now() - start, current.report};
}

/// The length of the UTF-8 sequence that starts a text and that XML 1.0 accepts, or 0.
inline std::size_t utf8Length(std::string_view text) {
    auto byte = [&](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    auto          lead = byte(0);
    std::size_t   length;
    std::uint32_t code;
    if (lead < 0x80)
        return 1;
    if (lead >= 0xc2 && lead <= 0xdf)
        length = 2, code = lead & 0x1fu;
    else if (lead >= 0xe0 && lead <= 0xef)
        length = 3, code = lead & 0x0fu;
    else if (lead >= 0xf0 && lead <= 0xf4)
        length = 4, code = lead & 0x07u;
    else
        return 0;
    if (text.size() < length)
        return 0;
    for (std::size_t index = 1; index != length; ++index) {
        if ((byte(index) & 0xc0) != 0x80)
            return 0;
        code = (code << 6) | (byte(index) & 0x3fu);
    }
    bool overlong = (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);
    bool excluded = (code >= 0xd800 && code <= 0xdfff) || code == 0xfffe || code == 0xffff || code > 0x10ffff;
    return overlong || excluded ? 0 : length;
}

/// Escapes a text for XML 1.0: the control characters it forbids, other than tab, line feed and carriage return, and
/// the bytes that are not valid UTF-8 are written as \xNN.
inline std::string xmlEscape(std::string_view text) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string                escaped;
    while (!text.empty()) {
        auto character = static_cast<unsigned char>(text.front());
        auto length    = utf8Length(text);
        if (character == '&')
            escaped += "&amp;";
        else if (character == '<')
            escaped += "&lt;";
        else if (character == '>')
            escaped += "&gt;";
        else if (character == '"')
            escaped += "&quot;";
        else if (length == 0 || (character < 0x20 && character != '\t' && character != '\n' && character != '\r')) {
            escaped += "\\x";
            escaped += digits[character >> 4];
            escaped += digits[character & 0xf];
            length   = 1;
        } else
            escaped += text.substr(0, length);
        text.remove_prefix(length);
    }
    return escaped;
}

inline bool writeJUnit(std::string_view path, const std::vector<Result>& results, double total) {
    std::size_t failed = 0, skipped = 0;
    for (const auto& result : results) {
        failed  += result.outcome == Outcome::failed;
        skipped += result.outcome == Outcome::skipped;
    }
    auto xml = concat("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n  <testsuite name=\"tests\" tests=\"",
                      results.size(),
                      "\" failures=\"",
                      failed,
                      "\" errors=\"0\" skipped=\"",
                      skipped,
                      "\" time=\"",
                      seconds(total),
                      "\">\n");
    for (const auto& result : results) {
        xml += concat("    <testcase classname=\"tests\" name=\"Scenario: ", xmlEscape(result.entry->title), "\" time=\"", seconds(result.seconds), '"');
        if (result.outcome == Outcome::passed)
            xml += "/>\n";
        else if (result.outcome == Outcome::failed)
            xml += concat(">\n      <failure message=\"failed\">", xmlEscape(result.report), "</failure>\n    </testcase>\n");
        else
            xml += ">\n      <skipped/>\n    </testcase>\n";
    }
    xml += "  </testsuite>\n</testsuites>\n";
    std::ofstream file{std::string{path}, std::ios::binary};
    file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
    file.close();
    return !file.fail();
}

inline bool number(std::string_view text, std::uint64_t& value) {
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

/// Reads the options; returns an error, or "help" when the usage was printed.
inline std::string parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        std::string_view argument = argv[index];
        std::string_view value;
        bool takesValue = argument == "--exact" || argument == "--junit" || argument == "--order" || argument == "--rng-seed" || argument == "--shard-count"
                          || argument == "--shard-index";
        if (takesValue) {
            if (index + 1 >= argc)
                return concat(argument, " needs a value");
            value = argv[++index];
        }
        if (argument == "--list")
            options.list = true;
        else if (argument == "--exact")
            options.exact = value;
        else if (argument == "--junit")
            options.junit = value;
        else if (argument == "--order") {
            if (value != "decl" && value != "rand")
                return "--order is decl or rand";
            options.randomOrder = value == "rand";
        } else if (argument == "--rng-seed") {
            if (!number(value, options.seed))
                return "--rng-seed needs a number";
        } else if (argument == "--shard-count") {
            if (!number(value, options.shardCount))
                return "--shard-count needs a number";
        } else if (argument == "--shard-index") {
            if (!number(value, options.shardIndex))
                return "--shard-index needs a number";
        } else if (argument == "--help" || argument == "-h") {
            print(concat("usage: ",
                         argv[0],
                         " [filter...] [--exact <title>] [--list] [--order decl|rand] [--rng-seed <n>]\n"
                         "       [--shard-count <n> --shard-index <i>] [--junit <file>]\n"
                         "A filter is a title (\"Scenario: \" optional, '*' at either end) or tags \"[a][b]\"; commas separate "
                         "alternatives.\n"));
            return "help";
        } else if (argument.starts_with("--"))
            return concat("unknown option ", argument);
        else
            options.filters.push_back(argument);
    }
    if (options.shardCount == 0 || options.shardIndex >= options.shardCount)
        return "--shard-index must be lower than --shard-count";
    return {};
}

/// A small generator for the random order (splitmix64).
inline std::uint64_t nextRandom(std::uint64_t& seed) {
    std::uint64_t value = (seed += 0x9e3779b97f4a7c15u);
    value               = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9u;
    value               = (value ^ (value >> 27)) * 0x94d049bb133111ebu;
    return value ^ (value >> 31);
}

}  // namespace impl

namespace impl {

/// Why CTest could not name a scenario: its title is written in a bracket argument, one per line of `--list`.
inline std::string_view titleError(std::string_view title) {
    if (title.empty())
        return "is empty";
    if (title.find_first_of("\r\n") != std::string_view::npos)
        return "holds a line break";
    if (title.find("]==]") != std::string_view::npos)
        return "holds ]==]";
    return {};
}

}  // namespace impl

/// Runs the selected scenarios. Returns 0 when they pass, 1 when one fails, 2 for a usage error or when nothing is
/// selected, and 4 when every selected scenario was skipped.
inline int run(int argc, char** argv) {
    using namespace impl;
    Options options;
    options.seed = static_cast<std::uint64_t>(now() * 1e9);
    if (auto error = parse(argc, argv, options); !error.empty()) {
        if (error == "help")
            return 0;
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    std::vector<const Entry*> selection;
    const auto&               entries = registry();
    for (std::size_t index = 0; index != entries.size(); ++index) {
        if (auto error = titleError(entries[index].title); !error.empty()) {
            std::fprintf(stderr, "%s\n", concat(entries[index].file, ':', entries[index].line, ": the title of a scenario ", error).c_str());
            return 2;
        }
        for (std::size_t previous = 0; previous != index; ++previous)
            if (entries[previous].title == entries[index].title) {
                std::fprintf(stderr,
                             "%s\n",
                             concat(entries[index].file,
                                    ':',
                                    entries[index].line,
                                    ": the scenario \"",
                                    entries[index].title,
                                    "\" is already declared at ",
                                    entries[previous].file,
                                    ':',
                                    entries[previous].line)
                                 .c_str());
                return 2;
            }
        if (selected(entries[index], options))
            selection.push_back(&entries[index]);
    }
    if (options.list) {
        std::string titles;
        for (const auto* entry : selection)
            titles += concat(entry->title, '\n');
        print(titles);
        return 0;
    }
    if (options.shardCount > 1) {
        std::vector<const Entry*> shard;
        for (auto index = options.shardIndex; index < selection.size(); index += options.shardCount)
            shard.push_back(selection[static_cast<std::size_t>(index)]);
        selection = std::move(shard);
    }
    if (selection.empty()) {
        std::fprintf(stderr, "No scenario matches the filters\n");
        return options.shardCount > 1 ? 0 : 2;
    }
    if (options.randomOrder) {
        print(concat("Randomized with seed ", options.seed, '\n'));
        auto seed = options.seed;
        for (auto index = selection.size(); index > 1; --index)
            std::swap(selection[index - 1], selection[static_cast<std::size_t>(nextRandom(seed) % index)]);
    }

    for (auto listener : listeners().runStarting)
        listener();
    std::vector<Result> results;
    results.reserve(selection.size());
    auto start = now();
    for (const auto* entry : selection)
        results.push_back(runScenario(*entry));
    auto elapsed = now() - start;

    std::size_t failed = 0, skipped = 0;
    for (const auto& result : results) {
        failed  += result.outcome == Outcome::failed;
        skipped += result.outcome == Outcome::skipped;
    }
    print(concat(failed ? "FAILED" : "All scenarios passed",
                 " (",
                 state().assertions.load(),
                 " assertions in ",
                 results.size(),
                 " scenarios: ",
                 results.size() - failed - skipped,
                 " passed, ",
                 failed,
                 " failed, ",
                 skipped,
                 " skipped)\n"));
    if (!options.junit.empty() && !writeJUnit(options.junit, results, elapsed)) {
        std::fprintf(stderr, "Cannot write %s\n", std::string{options.junit}.c_str());
        return 1;
    }
    if (failed)
        return 1;
    return skipped == results.size() ? 4 : 0;
}

}  // namespace webfront::bdd
