// CEF's macros must not reach a stdexec header (#243): WebFront.hpp must compile once they are defined.
#include <catch2/catch_test_macros.hpp>

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

SCENARIO("WebFront compiles after CEF's compiler macros") {
    GIVEN("the stdexec headers included before CEF's NO_UNIQUE_ADDRESS, ALWAYS_INLINE and NOINLINE") {
        THEN("every stdexec header WebFront.hpp includes was already included") {
            STATIC_REQUIRE(std::is_default_constructible_v<webfront::WebFront>);
        }
    }
}
