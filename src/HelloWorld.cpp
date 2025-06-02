#include <WebFront.hpp>
#include <system/BabelFS.hpp>
#include <system/IndexFS.hpp>
#include <system/NativeFS.hpp>
#include <system/ReactFS.hpp>

// CEF includes
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4100) // unreferenced parameter
#pragma warning(disable: 6011) // dereferencing NULL pointer - CEF internal issue
#endif
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/wrapper/cef_helpers.h"
#ifdef _WIN32
#include "include/cef_command_line.h"
#include "include/cef_sandbox_win.h"
#pragma warning(pop)
#endif

#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

// Forward declaration for openInDefaultBrowser function
auto openInDefaultBrowser(std::string_view port, std::string_view file) -> int;

// Simple CEF client implementation for embedded browser
class SimpleCEFClient : public CefClient,
                       public CefDisplayHandler,
                       public CefLifeSpanHandler,
                       public CefLoadHandler {
public:
    SimpleCEFClient() = default;

    // CefClient methods
    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    virtual CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    // CefDisplayHandler methods
    virtual void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString& title) override {
        // Set window title
        (void)browser; (void)title; // Suppress unused parameter warnings
    }

    // CefLifeSpanHandler methods
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        browser_ = browser;
        browser_count_++;
    }

    virtual bool DoClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser; // Suppress unused parameter warning
        // Allow the close - returning false allows the browser to close normally
        return false;
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser; // Suppress unused parameter warning
        
        // Decrement browser count
        browser_count_--;
        
        // Clear the browser reference if this was our browser
        if (browser_ && browser_->IsSame(browser)) {
            browser_ = nullptr;
        }
        
        // If this is the last browser, initiate proper shutdown sequence
        if (browser_count_ == 0) {
            browser_closed_ = true;
            
            // Schedule shutdown on a separate thread to avoid blocking CEF's cleanup
            std::thread([this]() {
                // Give CEF time to finish its internal cleanup
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                
                // Quit the message loop only after all browsers are closed
                CefQuitMessageLoop();
            }).detach();
        }
    }

    // CefLoadHandler methods
    virtual void OnLoadError(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           ErrorCode errorCode,
                           const CefString& errorText,
                           const CefString& failedUrl) override {
        // Handle load errors
        (void)browser; (void)frame; (void)errorCode; (void)errorText; (void)failedUrl; // Suppress unused parameter warnings
    }

    void CloseBrowser() {
        if (browser_) {
            browser_->GetHost()->CloseBrowser(false);
        }
    }

    bool IsBrowserClosed() const { return browser_closed_; }
    int GetBrowserCount() const { return browser_count_; }

private:
    CefRefPtr<CefBrowser> browser_;
    bool browser_closed_ = false;
    std::atomic<int> browser_count_{0};
    IMPLEMENT_REFCOUNTING(SimpleCEFClient);
};

// Simple CEF app implementation
class SimpleCEFApp : public CefApp, public CefBrowserProcessHandler {
public:
    SimpleCEFApp(const std::string& url) : url_(url) {}

    // CefApp methods
    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }    // CefBrowserProcessHandler methods
    virtual void OnContextInitialized() override {
        // CEF_REQUIRE_UI_THREAD(); // Removed wrapper dependency
        
        // Create the browser client
        CefRefPtr<SimpleCEFClient> client(new SimpleCEFClient());

        // Browser settings
        CefBrowserSettings browser_settings;        // Window info
        CefWindowInfo window_info;
#ifdef _WIN32
        // Create a windowed browser with default size
        window_info.SetAsPopup(nullptr, "WebFront CEF Browser");
        // Note: Width and height are set via SetAsPopup or can be controlled via window styles
#endif

        // Create the browser window
        CefBrowserHost::CreateBrowser(window_info, client, url_, browser_settings, nullptr, nullptr);
    }

private:
    std::string url_;
    IMPLEMENT_REFCOUNTING(SimpleCEFApp);
};

/// Open the web UI in CEF (Chromium Embedded Framework) browser
auto openInCEF(std::string_view port, std::string_view file) -> int {
    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);

#ifdef _WIN32
    // CEF main args (subprocess handling already done in main())
    CefMainArgs main_args(GetModuleHandle(nullptr));

    // CEF settings for main process only
    CefSettings settings;
    settings.no_sandbox = true;  // Disable sandbox for simplicity
    settings.multi_threaded_message_loop = false;  // Use single-threaded message loop
    
    // Disable CEF logging to reduce noise (but enable for debugging if needed)
    settings.log_severity = LOGSEVERITY_ERROR;  // Changed from DISABLE to ERROR to catch shutdown issues

    // Create the application
    CefRefPtr<SimpleCEFApp> app(new SimpleCEFApp(url));

    // Initialize CEF
    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        return -1;  // Failed to initialize CEF
    }

    // Run the message loop - this will block until CefQuitMessageLoop() is called
    CefRunMessageLoop();

    // Enhanced shutdown sequence
    std::cout << "CEF message loop exited, beginning shutdown sequence..." << std::endl;
    
    // Give CEF more time to clean up subprocesses properly
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Call CefShutdown to clean up CEF
    std::cout << "Calling CefShutdown()..." << std::endl;
    CefShutdown();
    
    std::cout << "CEF shutdown complete." << std::endl;

    return 0;
#else
    // For non-Windows platforms, fall back to default browser for now
    std::cout << "CEF implementation not yet available for this platform, opening in default browser..." << std::endl;
    return openInDefaultBrowser(port, file);
#endif
}

/// Open the web UI in the system's default browser
auto openInDefaultBrowser(std::string_view port, std::string_view file) -> int {
#ifdef _WIN32
    auto command = std::string("start ");
#elif __linux__
    auto command = std::string("xdg-open ");
#elif __APPLE__
    auto command = std::string("open ");
#endif

    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);
    return ::system(command.append(url).c_str());
}

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

#ifdef _WIN32
    // CEF subprocess handling must be done FIRST, before any other initialization
    CefMainArgs main_args(GetModuleHandle(nullptr));
    
    // Check if this is a CEF sub-process - if so, handle it and exit immediately
    int exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
    if (exit_code >= 0) {
        return exit_code;  // Sub-process completed, exit immediately without any WebFront setup
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
    std::thread serverThread([&webFront, &server_should_stop]() {
        webFront.run();
    });

    // Give the server a moment to start up
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    cout << "Starting CEF browser..." << endl;
    
    // Now launch CEF browser
    int cef_result = openInCEF(httpPort, mainHtml);
    
    cout << "CEF browser closed with result: " << cef_result << endl;

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