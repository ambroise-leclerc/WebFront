#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4100) // unreferenced parameter
#pragma warning(disable: 6011) // dereferencing NULL pointer - CEF internal issue
#endif
#include "cef_app.h"
#include "cef_browser.h"
#include "cef_client.h"
#include "wrapper/cef_helpers.h"
#ifdef _WIN32
#include "cef_command_line.h"
#include "cef_sandbox_win.h"
#pragma warning(pop)
#endif

#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <string>

// Forward declaration
namespace webfront {
auto openInDefaultBrowser(std::string_view port, std::string_view file) -> int;
}

namespace webfront {

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
            std::thread([]() {
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
        CefBrowserSettings browser_settings;
        
        // Window info
        CefWindowInfo window_info;
        
#ifdef _WIN32
        // Create a windowed browser with default size
        window_info.SetAsPopup(nullptr, "WebFront CEF Browser");
        // Note: Width and height are set via SetAsPopup or can be controlled via window styles
#elif __APPLE__
        // macOS window configuration - use windowless mode for better stability
        window_info.SetAsWindowless(0); // No parent window, offscreen rendering
        // Alternative child window approach (commented out due to potential stability issues):
        // CefRect bounds(0, 0, 1024, 768);
        // window_info.SetAsChild(0, bounds);
#elif __linux__
        // Linux window configuration
        window_info.SetAsChild(0, 0, 0, 1024, 768); // Parent window, x, y, width, height
        // Alternative: Use SetAsWindowless for offscreen rendering
        // window_info.SetAsWindowless(0);
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

#elif __APPLE__
    // macOS CEF implementation - step-by-step debugging
    std::cout << "=== CEF macOS Debug - Step 1: Starting..." << std::endl;
    
    try {
        std::cout << "=== CEF macOS Debug - Step 2: Creating main args..." << std::endl;
        CefMainArgs main_args(0, nullptr);
        
        std::cout << "=== CEF macOS Debug - Step 3: Creating settings..." << std::endl;
        CefSettings settings;
        settings.no_sandbox = true;
        settings.multi_threaded_message_loop = false;
        settings.log_severity = LOGSEVERITY_DISABLE; // Disable logging for now
        
        std::cout << "=== CEF macOS Debug - Step 4: About to call CefInitialize..." << std::endl;
        
        // Test minimal CEF initialization without app
        if (!CefInitialize(main_args, settings, nullptr, nullptr)) {
            std::cout << "=== CEF macOS Debug - Step 5: CefInitialize failed, fallback..." << std::endl;
            return openInDefaultBrowser(port, file);
        }
        
        std::cout << "=== CEF macOS Debug - Step 6: CefInitialize succeeded!" << std::endl;
        
        // Immediate shutdown to test basic initialization
        std::cout << "=== CEF macOS Debug - Step 7: Calling CefShutdown..." << std::endl;
        CefShutdown();
        
        std::cout << "=== CEF macOS Debug - Step 8: CefShutdown complete, using fallback..." << std::endl;
        return openInDefaultBrowser(port, file);
        
    } catch (const std::exception& e) {
        std::cout << "=== CEF macOS Debug - Exception caught: " << e.what() << std::endl;
        return openInDefaultBrowser(port, file);
    } catch (...) {
        std::cout << "=== CEF macOS Debug - Unknown exception caught" << std::endl;
        return openInDefaultBrowser(port, file);
    }
    
#elif __linux__
    // Linux CEF implementation
    std::cout << "Starting CEF browser on Linux..." << std::endl;
    
    // CEF main args - on Linux, we pass command line arguments
    CefMainArgs main_args(0, nullptr);

    // CEF settings for Linux
    CefSettings settings;
    settings.no_sandbox = true;  // Disable sandbox for simplicity
    settings.multi_threaded_message_loop = false;  // Use single-threaded message loop
    
    // Set log severity
    settings.log_severity = LOGSEVERITY_ERROR;
    
    // Create the application
    CefRefPtr<SimpleCEFApp> app(new SimpleCEFApp(url));

    // Initialize CEF
    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::cout << "Failed to initialize CEF on Linux, falling back to default browser..." << std::endl;
        return openInDefaultBrowser(port, file);
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
    // For other platforms, fall back to default browser
    std::cout << "CEF implementation not yet available for this platform, opening in default browser..." << std::endl;
    return openInDefaultBrowser(port, file);
#endif
}

} // namespace webfront
