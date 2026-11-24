// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-16
//
// WebFront — tests/unit
// Role: CEF's macros must not reach a stdexec header: WebFront.hpp compiles once they are defined.
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: compile-time regression; see SenderHeaders.hpp

// CEF's macros must not reach a stdexec header (#243): WebFront.hpp must compile once they are defined.

#include <webfront/testing/Bdd.hpp>

#ifndef WEBFRONT_EMBED_CEF
    // frontend/CEF.hpp includes the stdnet and stdexec headers before CEF's headers, which define these
    // macros as include/base/cef_compiler_specific.h does (GCC and Clang with NDEBUG, MSVC).
    #include <webfront/networking/SenderHeaders.hpp>

    #ifdef _MSC_VER
        #define NOINLINE      __declspec(noinline)
        #define ALWAYS_INLINE __forceinline
    #else
        #define NOINLINE      __attribute__((noinline))
        #define ALWAYS_INLINE inline __attribute__((__always_inline__))
    #endif
    #define NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#include <webfront/WebFront.hpp>

#include <type_traits>

using namespace webfront::bdd;

const Scenario webFrontCompiles{"WebFront compiles after CEF's compiler macros", [] {
                                    given("the stdexec headers included before CEF's NO_UNIQUE_ADDRESS, ALWAYS_INLINE and NOINLINE", [&] {
                                        then("every stdexec header WebFront.hpp includes was already included", [&] {
                                            static_assert(std::is_default_constructible_v<webfront::WebFront>);
                                        });
                                    });
                                }};
