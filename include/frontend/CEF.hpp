#pragma once

#include <string_view>

#ifdef WEBFRONT_HAS_CEF
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/wrapper/cef_helpers.h"

class SimpleCEFClient : public CefClient,
                        public CefDisplayHandler,
                        public CefLifeSpanHandler {
public:
    SimpleCEFClient() = default;

    // CefClient methods
    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }

    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }    // CefDisplayHandler methods
    virtual void OnTitleChange(CefRefPtr<CefBrowser> browser,
                             const CefString& title) override {
        // Handle title changes if needed
        (void)browser;
        (void)title;
    }

    // CefLifeSpanHandler methods
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        // Browser has been created
        (void)browser;
    }

    virtual bool DoClose(CefRefPtr<CefBrowser> browser) override {
        // Allow the close
        (void)browser;
        return false;
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        // Browser is closing
        (void)browser;
    }

private:
    IMPLEMENT_REFCOUNTING(SimpleCEFClient);
};

class SimpleCEFApp : public CefApp {
public:
    SimpleCEFApp() = default;

private:
    IMPLEMENT_REFCOUNTING(SimpleCEFApp);
};

inline auto openInCEF(std::string_view port, std::string_view file) -> int {    // CEF command line arguments
    CefMainArgs main_args;    // CEF settings
    CefSettings settings;
    settings.no_sandbox = true;
    // Note: single_process mode is not available in this CEF version

#ifdef __APPLE__
    // macOS-specific CEF settings
    settings.multi_threaded_message_loop = false;  // Use single-threaded message loop on macOS
#endif

    // Initialize CEF
    CefRefPtr<SimpleCEFApp> app(new SimpleCEFApp);
    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        return -1;
    }    // Create browser window info
    CefWindowInfo window_info;
#ifdef _WIN32
    // Windows-specific window creation
    window_info.SetAsPopup(nullptr, "WebFront");
#elif defined(__APPLE__)
    // macOS-specific window creation - use default settings
    // CefWindowInfo on macOS doesn't have width/height properties
    // The window will be created with default size by CEF
    // We can set window bounds if needed using SetBounds()
    // window_info.SetBounds(0, 0, 1024, 768);  // This might work on some versions
#else
    // Linux/other platforms - attempt default windowed mode
    // Different CEF versions/platforms may have different properties available
#endif

    // Browser settings
    CefBrowserSettings browser_settings;

    // Create the browser client
    CefRefPtr<SimpleCEFClient> client(new SimpleCEFClient);

    // Build the URL
    std::string url = "http://localhost:" + std::string(port) + "/" + std::string(file);

    // Create the browser
    CefBrowserHost::CreateBrowser(window_info, client, url, browser_settings, nullptr, nullptr);

    // Run the CEF message loop
    CefRunMessageLoop();

    // Shutdown CEF
    CefShutdown();

    return 0;
}

#else

inline auto openInCEF(std::string_view port, std::string_view file) -> int {
    // CEF not available, return error code
    return -1;
}

#endif // WEBFRONT_HAS_CEF