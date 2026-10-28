// Check before standard headers can supply their vendor defaults.
#if defined(_GLIBCXX_ASSERTIONS) || defined(_LIBCPP_HARDENING_MODE) || defined(_MSVC_STL_HARDENING)
#error WebFront must not impose standard library hardening on consumers
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    #error WebFront must not impose sanitizers on consumers
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#error WebFront must not impose sanitizers on consumers
#endif
#endif

#include <webfront/WebFront.hpp>
#include <webfront/testing/Jasmine.hpp>

int main(int argc, char**) {
    if (argc > 1) {  // never taken by the smoke test: it only proves that the library compiles and links
        webfront::WebFront app;
        webfront::testing::JasmineTest tests{app};
        app.show();
    }
    return 0;
}
