#include <thread>
#include <chrono>
#include <filesystem>
#include <iostream>

#include <WebFront.hpp>
#include <system/BabelFS.hpp>
#include <system/IndexFS.hpp>
#include <system/NativeFS.hpp>
#include <system/ReactFS.hpp>
#include <frontend/CEF.hpp>

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
            webfront = webfront / "src";
            log::info("  not found : Looking now in source directory {}", webfront.string());
            if (exists(webfront / filename)) return webfront;
        }
    }

    throw runtime_error("Cannot find " + filename + " file");
}

int main(int /*argc*/, char** /*argv*/) {
    using HelloFS = fs::Multi<fs::NativeDebugFS, fs::IndexFS, fs::ReactFS, fs::BabelFS>;
    using WebFrontDbg = BasicWF<NetProvider, HelloFS>;


    auto httpPort = "9002";
    auto mainHtml = "react.html";
    auto docRoot = findDocRoot(mainHtml);

    cout << "WebFront launched from " << filesystem::current_path().string() << "\n";
    log::setLogLevel(log::Debug);
    log::addSinks(log::clogSink);
    WebFrontDbg webFront(httpPort, docRoot);

    webFront.cppFunction<void, std::string>("print", [](std::string text) { std::cout << text << '\n'; });
      webFront.onUIStarted([](WebFrontDbg::UI ui) {
        ui.addScript("var addText = function(text, num) {                 \n"
                     "  let print = webFront.cppFunction('print');        \n"
                     "  print(text + ' of ' + num);                       \n"
                     "}                                                   \n"
                     "                                                    \n"
                     "var testFunc = function(text) {                     \n"
                     "  let bigText = 'bigText : ' + text + text + ' - '; \n"
                     "  bigText += bigText + bigText;                     \n"
                     "  let cppTest = webFront.cppFunction('cppTest');    \n"
                     "  cppTest(text, bigText, bigText.length);           \n"
                     "}                                                   \n");
        auto print = ui.jsFunction("addText");
        print("Hello World", 2025);
        ui.jsFunction("testFunc")("Texte de test suffisament long pour changer de format");
    });

    // Start the HTTP server in a background thread
    std::thread serverThread([&webFront]() {
        webFront.run();
    });

    webFront.openWindow(mainHtml);

    log::info("Application window closed");

    // When CEF closes, stop the server cleanly
    webFront.stop();

    log::info("Waiting for server thread to finish...");
    
    // Wait for the server thread to finish
    if (serverThread.joinable()) {
        serverThread.join();
    }

    log::info("Application shutdown complete.");

    return 0;
}