#include <WebFront.hpp>
#include <frontend/DefaultBrowser.hpp>
#include <frontend/CEF.hpp>

#include <system/IndexFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/NativeFS.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace std;
using namespace webfront;

// Find the DocRoot path containing the given file (look for the filename in current directory,
// in temp directory then in sources directory
//
// @param filename
// @return DocRoot path
filesystem::path findDocRoot(string filename) {
    using namespace filesystem;

    log::info("Looking for {} in {}", filename, current_path().string());
    if (exists(current_path() / filename)) return current_path();

    log::info("  not found : Looking now in temp directory {} ", temp_directory_path().string());
    if (exists(temp_directory_path() / filename)) return temp_directory_path();

    path webfront, cp{current_path()};
    for (auto element = cp.begin(); element != cp.end(); ++element) {
        webfront = webfront / *element;
        if (*element == "WebFront") {
            webfront = webfront / "webtest";
            log::info("  not found : Looking now in source directory {}", webfront.string());
            if (exists(webfront / filename)) return webfront;
        }
    }

    throw runtime_error("Cannot find " + filename + " file");
}

int main(int /*argc*/, char** /*argv*/) {
    // Initialize CEF and handle subprocesses
    int cef_result = webfront::cef::initialize();
    if (cef_result != 0) {
        // Either error (-1) or subprocess (>0) - exit either way
        return cef_result;
    }
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);

    std::string_view httpPort{"9002"};
    using Jas = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::JasmineFS>;

    try {
        auto specRunnerFile = "SpecRunner.html";
        auto docRoot = findDocRoot(specRunnerFile);

        using WF = BasicWF<NetProvider, Jas>;
        WF webFront(httpPort, docRoot);
        webFront.cppFunction<std::string, std::string>("getVersion", [](std::string) { return webfront::version; });
        webFront.onUIStarted([](WF::UI ui) {
            log::info("UI Started");
            ui.addScript("var addText = function(text, num) {                 \n"
                     "  displayText(text + ' of ' + num);                       \n"
                     "}                                                   \n");
                     auto displayText = ui.jsFunction("addText");
                     displayText("Hello World", 2023);
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