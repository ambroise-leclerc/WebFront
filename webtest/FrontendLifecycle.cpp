#include <system/NativeFS.hpp>
#include <WebFront.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifndef WEBFRONT_TEST_FRONTEND
    #error WEBFRONT_TEST_FRONTEND must select an embedded frontend
#endif

namespace {
using Frontend = WEBFRONT_TEST_FRONTEND;
using TestFS   = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
using App      = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, Frontend>;
static_assert(Frontend::action == webfront::frontend::Action::closeServerAfterOpen);

int show(std::string_view page) {
    App               app("9002", std::filesystem::current_path());
    std::atomic<bool> connected = false;
    app.onUIStarted([&](App::UI) {
        connected = true;
    });
    app.show(page);
    if (connected)
        throw std::runtime_error("Unexpected bridge connection");
    return 0;
}

int expectNavigationFailure() {
    try {
        Frontend::open("0", "index.html");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 0;  // The contract requires an exception, without prescribing its wording.
    }
    throw std::runtime_error("Failed initial navigation did not throw");
}

int run(std::string_view mode) {
    if (mode == "no-bridge")
        return show("no-bridge.html");
    if (mode == "early-close")
        return show("no-bridge.html?early-close");
    if (mode == "rejected-ready")
        return show("rejected-ready.html");
    if (mode == "path-encoding")
        return show("encoded path %23.html?x=1#example");
    if (mode == "navigation-failure")
        return expectNavigationFailure();
    throw std::runtime_error("Unknown lifecycle scenario");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        // CEF subprocesses have their own argument list and must dispatch before mode parsing.
        webfront::detail::ensureFrontendInitialized<Frontend>();
        return argc == 2 ? run(argv[1]) : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
