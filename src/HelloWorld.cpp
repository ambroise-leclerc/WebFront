#include <thread>
#include <chrono>

#include <WebFront.hpp>
#include <system/BabelFS.hpp>
#include <system/IndexFS.hpp>
#include <system/NativeFS.hpp>
#include <system/ReactFS.hpp>

#ifdef WEBFRONT_EMBED_CEF
#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"
#endif

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

int main(int argc, char** argv) {
#ifdef WEBFRONT_EMBED_CEF
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
#else
    // Suppress unused parameter warnings when CEF is not available
    (void)argc;
    (void)argv;
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

    webFront.openWindow(mainHtml);

    log::info("Application window closed");

    // When CEF closes, signal the server to stop
    server_should_stop = true;

    log::info("Waiting for server thread to finish...");
    
    // Wait for the server thread to finish
    if (serverThread.joinable()) {
        serverThread.join();
    }

    log::info("Application shutdown complete.");

    return 0;
}