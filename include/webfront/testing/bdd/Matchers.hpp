// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — testing
// Role: The string matchers of webfront::bdd and the assertions that check a text against a matcher.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public maintained interface (docs/BDD.md); optional part of the framework, built on bdd/Core.hpp only

// Matches a text against what a test expects of it: require(text, contains("lo, w")). requireThrows() accepts the same
// matchers for the message of an exception. Any type that satisfies webfront::bdd::Matcher works with both.

#pragma once

#include <webfront/testing/bdd/Core.hpp>

namespace webfront::bdd {

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

}  // namespace webfront::bdd
