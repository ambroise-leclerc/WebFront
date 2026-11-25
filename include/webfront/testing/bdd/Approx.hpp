// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — testing
// Role: approx(), a floating-point value that compares equal within a relative epsilon or an absolute margin.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public maintained interface (docs/BDD.md); optional part of the framework, Catch2's Approx semantics

// Compares a measured value with an expected one within a tolerance, as Catch2's Approx does:
// @code
// check(reading == approx(5.0));                    // within a relative epsilon of about 1.2e-5
// check(reading == approx(5.0).margin(1e-6));       // or within an absolute margin
// checkEqual(reading, approx(5.0).epsilon(1e-3));   // reports both operands when it fails
// @endcode
// A value is accepted when it is within either tolerance: |value - expected| <= margin, or
// |value - expected| <= epsilon * |expected| (0 for an infinite expected value). A failed check() names the value, the
// expected value, both tolerances and the difference.

#pragma once

#include <webfront/testing/bdd/Core.hpp>

#include <cstdio>
#include <limits>
#include <stdexcept>

namespace webfront::bdd {

/// An expected floating-point value and the tolerances within which a value equals it.
class Approx {
public:
    /// The default relative tolerance, Catch2's: a hundred times the epsilon of a float, about 1.2e-5.
    static constexpr double defaultEpsilon = static_cast<double>(std::numeric_limits<float>::epsilon()) * 100.0;

    explicit Approx(double value) : expected{value} { }

    /// Sets the relative tolerance, a fraction of the expected value. Throws std::invalid_argument when it is negative or NaN.
    Approx& epsilon(double relative) {
        relativeTolerance = valid(relative, "epsilon");
        return *this;
    }
    /// Sets the absolute tolerance. Throws std::invalid_argument when it is negative or NaN.
    Approx& margin(double absolute) {
        absoluteTolerance = valid(absolute, "margin");
        return *this;
    }

    double value() const {
        return expected;
    }
    double epsilon() const {
        return relativeTolerance;
    }
    double margin() const {
        return absoluteTolerance;
    }

    /// Whether a value is within either tolerance of the expected value. NaN is never.
    bool matches(double other) const {
        constexpr double infinity = std::numeric_limits<double>::infinity();
        auto             within   = [&](double tolerance) {
            return other + tolerance >= expected && expected + tolerance >= other;
        };
        bool infinite = expected == infinity || expected == -infinity;
        return within(absoluteTolerance) || within(relativeTolerance * (infinite ? 0.0 : (expected < 0 ? -expected : expected)));
    }

private:
    static double valid(double tolerance, const char* name) {
        if (!(tolerance >= 0.0))
            throw std::invalid_argument{impl::concat("approx: the ", name, " must be zero or positive, not ", tolerance)};
        return tolerance;
    }

    double expected;
    double relativeTolerance = defaultEpsilon;
    double absoluteTolerance = 0.0;
};

/// An expected value compared within a tolerance: `check(reading == approx(5.0).margin(1e-6))`.
inline Approx approx(double expected) {
    return Approx{expected};
}

/// Writes approx(5) (margin 1e-06, epsilon 1.19209e-05) in messages and in the operands of a failed checkEqual().
template <>
struct Printer<Approx> {
    static void append(std::string& text, const Approx& approx) {
        auto tolerance = [](double value) {
            char buffer[32];
            auto length = std::snprintf(buffer, sizeof buffer, "%g", value);
            return impl::withPoint({buffer, length > 0 ? static_cast<std::size_t>(length) : 0});
        };
        text += impl::concat("approx(", approx.value(), ") (margin ", tolerance(approx.margin()), ", epsilon ", tolerance(approx.epsilon()), ')');
    }
};

/// The result of comparing a value with approx(): true or false like a bool, and able to explain a failed check().
class ApproxComparison {
public:
    ApproxComparison(double value, const Approx& approx, bool equal) : actual{value}, expected{approx}, equality{equal} { }

    operator bool() const {
        return expected.matches(actual) == equality;
    }
    std::string explain() const {
        auto difference = actual - expected.value();
        return impl::concat(actual,
                            equality ? " == " : " != ",
                            expected,
                            equality ? ": the difference is " : ": the values are within tolerance, the difference is ",
                            difference < 0 ? -difference : difference);
    }

private:
    double actual;
    Approx expected;
    bool   equality;
};

namespace impl {

/// A number approx() compares: any arithmetic type but bool.
template <typename T>
concept Approximable = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

}  // namespace impl

template <impl::Approximable T>
ApproxComparison operator==(const T& actual, const Approx& expected) {
    return {static_cast<double>(actual), expected, true};
}
template <impl::Approximable T>
ApproxComparison operator==(const Approx& expected, const T& actual) {
    return {static_cast<double>(actual), expected, true};
}
template <impl::Approximable T>
ApproxComparison operator!=(const T& actual, const Approx& expected) {
    return {static_cast<double>(actual), expected, false};
}
template <impl::Approximable T>
ApproxComparison operator!=(const Approx& expected, const T& actual) {
    return {static_cast<double>(actual), expected, false};
}

}  // namespace webfront::bdd
