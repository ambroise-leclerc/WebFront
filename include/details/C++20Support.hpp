/// @date 29/01/2022 13:58:42
/// @author Ambroise Leclerc
/// @brief C++20 missing functionalities for selected targets
#pragma once

#include <version>
#ifndef __cpp_concepts
#pragma message("Concepts minimal implementation added")
#include <concepts>
#include <type_traits>
namespace std {
template<typename T, typename... Args>
concept constructible_from = destructible<T> && is_constructible_v<T, Args...>;

template<typename T, typename U>
concept convertible_to = is_convertible_v<T, U> && requires(add_rvalue_reference_t<T> (&t)()) {
    static_cast<U>(t());
};

template<class T>
concept swappable = requires(T& t, T& t2) {
    swap(t, t2);
};

template<class T>
concept move_constructible = std::constructible_from<T, T> && std::convertible_to<T, T>;

template<class T>
concept movable = std::is_object_v<T> && std::move_constructible<T> && std::swappable<T>;
} // namespace std
#endif

#ifdef __cpp_lib_format
#include <format>
#else
#pragma message("std::format minimal implementation added")
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace webfront {
template<typename C, typename T>
std::ostream& operator<<(std::ostream& os, std::chrono::time_point<C, T> t) {
    using namespace std::chrono;
    auto tp = t.time_since_epoch();
    auto d = duration_cast<days>(tp);
    tp -= d;
    auto h = duration_cast<hours>(tp);
    tp -= h;
    auto m = duration_cast<minutes>(tp);
    tp -= m;
    auto s = duration_cast<seconds>(tp);
    tp -= s;
    auto us = duration_cast<microseconds>(tp);
    os << std::setfill('0') << std::setw(2) << h.count() << ':';
    os << std::setfill('0') << std::setw(2) << m.count() << ':';
    os << std::setfill('0') << std::setw(2) << s.count() << '.' << std::setw(6) << us.count();
    return os;
}
} // namespace webfront
namespace std {
string format(std::string_view fmt, auto&&... ts) {
    if (sizeof...(ts) == 0) return std::string(fmt);
    std::array<std::string_view, 32> fragments{};
    if (sizeof...(ts) > fragments.size()) throw std::length_error("format parameters count limit exceeded");
    size_t index = 0;
    auto start = fmt.cbegin(), parser = start;
    while (parser != fmt.cend()) {
        if (*parser == '{') {
            if (start == fmt.cbegin())
                fragments[index++] = std::string_view(start, static_cast<size_t>(parser - start));          /// string_view constructor with iterators is missing in Apple Clang 13
            else if (start != parser && index < fragments.size())
                fragments[index++] = std::string_view(start + 1, static_cast<size_t>(parser - (start + 1)));/// string_view constructor with iterators is missing in Apple Clang 13
        }
        else if (*parser == '}')
            start = parser;
        ++parser;
    }
    if (start + 1 != fmt.cend()) fragments[index++] = std::string_view(start + 1, static_cast<size_t>(fmt.cend() - (start + 1)));/// string_view constructor with iterators is missing in Apple Clang 13

    auto frag = fragments.cbegin();
    stringstream ss;
    using webfront::operator<<;
    ((ss << *frag++ << std::forward<decltype(ts)>(ts)), ...);
    while (frag != fragments.cend()) ss << *frag++;
    return ss.str();
}



} // namespace std

#endif

#ifdef __cpp_lib_source_location
#include <source_location>
#else
#pragma message("std::source_location minimal implementation added")
namespace std {
struct source_location {
    static source_location current() noexcept { return {}; }
    constexpr source_location() noexcept {}

    constexpr uint_least32_t line() const noexcept { return l; };
    constexpr uint_least32_t column() const noexcept { return c; };
    constexpr const char* file_name() const noexcept { return "file"; }
    constexpr const char* function_name() const noexcept { return "function"; }

private:
    uint_least32_t l = 0;
    uint_least32_t c = 0;
};
} // namespace std
#endif
