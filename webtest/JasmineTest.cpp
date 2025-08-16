#include <WebFront.hpp>
#include <frontend/DefaultBrowser.hpp>
#include <frontend/CEF.hpp>

#include <system/IndexFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/NativeFS.hpp>
#include <tooling/PathUtils.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace std;
using namespace webfront;


int main(int /*argc*/, char** /*argv*/) {
    // Initialize CEF and handle subprocesses
    try {
        webfront::cef::initialize();
    } catch (const webfront::cef::CEFSubprocessExit& e) {
        // If this is a CEF subprocess, exit with the provided code
        return e.exit_code();
    } catch (const webfront::cef::CEFInitializationError& e) {
        std::cerr << e.what() << std::endl;
        return -1;
    }
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);

    std::string_view httpPort{"9002"};
    using Jas = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::JasmineFS>;

    try {
        auto specRunnerFile = "SpecRunner.html";
        auto docRoot = tooling::findTestRoot(specRunnerFile);

        using WF = BasicWF<NetProvider, Jas>;
        WF webFront(httpPort, docRoot);
        // Register C++ functions for testing
        webFront.cppFunction<std::string, std::string>("getVersion", [](std::string) { 
            return webfront::version; 
        });
        
        webFront.onUIStarted([](WF::UI) {
            log::info("Jasmine test runner UI started");
            // Test environment ready - Jasmine will execute tests automatically
        });

        // Start the HTTP server in a background thread
        std::thread serverThread([&webFront]() {
            webFront.run();
        });

        log::info("Starting Jasmine test runner...");
        webFront.openWindow(specRunnerFile);
        log::info("Test runner window closed");

        // When window closes, stop the server cleanly
        webFront.stop();

        // Wait for server thread to finish
        if (serverThread.joinable()) {
            serverThread.join();
        }
    }
    catch (std::runtime_error& e) {
        log::error("runtime_error : {}", e.what());
    }
    return 0;
}