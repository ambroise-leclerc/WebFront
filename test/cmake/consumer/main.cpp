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

#if defined(WEBFRONT_CONSUMER_EXPECT_NATIVE) && !defined(WEBFRONT_NATIVE_WINDOW)
#error The native consumer must compile the native frontend branch
#endif
#if defined(WEBFRONT_NATIVE_WINDOW)
using ConsumerApp = webfront::WebFrontWithFrontend<webfront::frontend::NativeWebViewFrontend>;
#else
using ConsumerApp = webfront::WebFront;
#endif

int main(int argc, char**) {
    if (argc > 1) {  // never taken by the smoke test: it only proves that the library compiles and links
        ConsumerApp app;
        webfront::testing::JasmineTest tests{app};
        app.show();
    }
    return 0;
}
