/// @date 29/01/2022 13:58:42
/// @author Ambroise Leclerc
/// @brief C++20 missing functionnalities for selected targets
#pragma once

#ifdef __APPLE__
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
concept move_constructible = constructible_from<T, T> && convertible_to<T, T>;

template<class T>
concept movable = is_object_v<T> && move_constructible<T> && swappable<T>;
} // namespace std
#endif

#if __has_include(<format>)
#include <format>
#else
#include <chrono>
#include <iomanip>
#include <sstream>

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
    tp -= s;
    os << std::setfill('0') << std::setw(2) << h.count() << ':' << m.count() << ':' << s.count() << '.' << std::setw(6) << us.count();
    return os;
}
} // namespace webfront
namespace std {
template<typename... Ts>
string format(string_view, Ts&&... ts) {
    using webfront::operator<<;
    stringstream ss;
    ((ss << std::forward<Ts>(ts) << ' '), ...);
    return ss.str();
}
} // namespace std

#endif

#if __has_include(<source_location>)
#include <source_location>
#else
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
