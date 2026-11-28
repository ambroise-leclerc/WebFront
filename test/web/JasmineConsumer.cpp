// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-28
//
// WebFront — tests/web
// Role: Program that runs the Jasmine instrumentation of a consumer's real page.
// Thread: ui-loop | Alloc: unrestricted
// LLM: test support; see Jasmine.hpp

#include "../driver/WfDriver.hpp"
#include <webfront/testing/Jasmine.hpp>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    using Frontend = WEBFRONT_DRIVER_FRONTEND;
    // A failure before the report (the configuration, the server, the engine) is said and ends with status 1, which
    // RunWebFrontJasmine.cmake reports with this message; uncaught, it would end the process with no word (#707).
    try {
        // First: CEF runs its subprocesses (GPU, network, renderer) from this executable, with their own arguments, and
        // they must end in this call before main() looks at the arguments.
        webfront::detail::ensureFrontendInitialized<Frontend>();
        if (argc != 2)
            return 64;
        using App = webfront::
            BasicWF<webfront::NetProvider, webfront::fs::DefaultFS, webfront::http::DefaultBuffersPolicy, webfront::testdriver::DriverFrontend<Frontend>>;
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
    } catch (const std::exception& error) {
        std::cerr << "jasmine_consumer: " << error.what() << '\n';
        return 1;
    }
}
