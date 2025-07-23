#include <WebFront.hpp>
#include <frontend/DefaultBrowser.hpp>
#include <frontend/CEF.hpp>
#include <system/BabelFS.hpp>
#include <system/IndexFS.hpp>
#include <system/NativeFS.hpp>
#include <system/ReactFS.hpp>
#include <thread>
#include <chrono>

#ifdef WEBFRONT_HAS_CEF
#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"
#endif

using namespace std;
using namespace webfront;

// Simple server readiness check using time-based approach
void waitForServerStartup() {
    cout << "Waiting for HTTP server to start..." << endl;
    
    // Give the server adequate time to start up
    for (int i = 1; i <= 3; ++i) {
        cout << "Server startup... " << i << "/3" << endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    
    cout << "✅ Server should be ready now" << endl;
}

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

int main(int argc, char** argv) {
#ifdef WEBFRONT_HAS_CEF
    // Set environment variables BEFORE any CEF operations
    setenv("DISABLE_KEYCHAIN_ACCESS", "1", 1);
    setenv("OSX_DISABLE_KEYCHAIN", "1", 1);  
    setenv("USE_MOCK_KEYCHAIN", "1", 1);
    setenv("CHROME_KEYCHAIN_REAUTH_DISABLED", "1", 1);
    setenv("PASSWORD_MANAGER_ENABLED", "0", 1);
    
    // Load the CEF framework library at runtime - required on macOS
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) {
        cout << "Failed to load CEF library" << endl;
        return -1;
    }
    
    // CEF subprocesses require CefExecuteProcess to be called first
    CefMainArgs main_args(argc, argv);
    
    // Create a simple app for subprocess handling
    CefRefPtr<CefApp> app;
    
    // Execute the subprocess if this is a helper process
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        // If this is a subprocess, exit immediately
        return exit_code;
    }
#endif

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
        print("Hello World", 2022);
        ui.jsFunction("testFunc")("Texte de test suffisament long pour changer de format");
    });    // Start the HTTP server in a background thread
    std::atomic<bool> server_should_stop{false};
    std::thread serverThread([&webFront]() {
        webFront.run();
    });    
    
    // Wait for the server to be fully ready
    waitForServerStartup();

#ifdef WEBFRONT_HAS_CEF
    cout << "Server ready - Starting CEF browser..." << endl;
    
    // Now launch CEF browser
    int cef_result = openInCEF(httpPort, mainHtml);
    
    cout << "CEF browser closed with result: " << cef_result << endl;
#else
    cout << "CEF not available, starting default browser..." << endl;
    
    // Launch default browser when CEF is not available
    int cef_result = openInDefaultBrowser(httpPort, mainHtml);
    
    cout << "Default browser opened with result: " << cef_result << endl;
#endif

    // When CEF closes, signal the server to stop
    server_should_stop = true;
    
    cout << "Waiting for server thread to finish..." << endl;
    
    // Wait for the server thread to finish
    if (serverThread.joinable()) {
        serverThread.join();
    }
    
    cout << "Application shutdown complete." << endl;

    return cef_result;
}