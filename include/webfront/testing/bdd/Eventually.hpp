// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — testing
// Role: The waits of webfront::bdd: an observable condition or a future, within a delay scaled by WEBFRONT_TEST_TIMEOUT_FACTOR.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public maintained interface (docs/BDD.md); optional part of the framework, the only one that needs <chrono> and <thread>

// Waits for an asynchronous state instead of sleeping a fixed time: requireEventually, checkEventually, waitUntil,
// requireReady, checkReady and waitReady. A loaded machine only makes them take longer.

#pragma once

#include <webfront/testing/bdd/Core.hpp>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace webfront::bdd {

namespace impl {

/// A condition seen through a pointer, like Body. It points to a callable of the caller that must outlive it: the public
/// functions build it from a lambda of their own, which also accepts free functions.
struct Condition {
    void* object        = nullptr;
    bool (*call)(void*) = nullptr;

    template <typename F>
    explicit Condition(F& predicate)
        : object{__builtin_addressof(predicate)}, call{[](void* target) {
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
    using Clock             = std::chrono::steady_clock;
    const auto     start    = Clock::now();
    const auto     deadline = start + std::chrono::duration_cast<Clock::duration>(limit * timeoutFactor());
    constexpr auto longest  = std::chrono::microseconds{5000};
    auto           pause    = std::chrono::microseconds{100};
    for (;;) {
        if (condition())
            return {std::chrono::duration<double>(Clock::now() - start).count(), true};
        if (Clock::now() >= deadline)
            return {std::chrono::duration<double>(Clock::now() - start).count(), false};
        std::this_thread::sleep_for(pause);
        pause = pause * 2 < longest ? pause * 2 : longest;
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

}  // namespace webfront::bdd
