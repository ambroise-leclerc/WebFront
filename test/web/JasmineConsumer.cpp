#include "../driver/WfDriver.hpp"
#include <webfront/testing/Jasmine.hpp>

int main(int argc, char** argv) {
    using Frontend = WEBFRONT_DRIVER_FRONTEND;
    webfront::detail::ensureFrontendInitialized<Frontend>();
    if (argc != 2)
        return 64;
    using App =
        webfront::BasicWF<webfront::NetProvider, webfront::fs::DefaultFS, webfront::http::DefaultBuffersPolicy, webfront::testdriver::DriverFrontend<Frontend>>;
    App app{
        webfront::WebFrontConfig{.port = "0", .documentRoot = argv[1]}
    };
    app.cppFunction("consumer.answer", [] {
        return 42;
    });
    webfront::testing::JasmineTest tests{app};
    app.show();
    app.shutdown();
    return tests.exitCode();
}
