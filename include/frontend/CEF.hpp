#pragma once

#include <string_view>
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>

#ifdef WEBFRONT_HAS_CEF
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"

// CEF already provides IMPLEMENT_REFCOUNTING macro

class SimpleCEFWindowDelegate : public CefWindowDelegate {
public:
    SimpleCEFWindowDelegate() = default;

    // CefWindowDelegate methods
    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        // Add the browser view to the window
        window->AddChildView(browser_view_);
        
        // Set window properties for a clean application window
        window->SetTitle("WebFront Application");
        window->CenterWindow(CefSize(1200, 800));
        window->Show();
    }

    void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
        (void)window; // Suppress unused parameter warning
        browser_view_ = nullptr;
    }

    bool CanClose(CefRefPtr<CefWindow> window) override {
        (void)window; // Suppress unused parameter warning
        // Allow the window to close, which will terminate the app
        return true;
    }

    CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
        (void)view; // Suppress unused parameter warning
        return CefSize(1200, 800);
    }

    void SetBrowserView(CefRefPtr<CefBrowserView> browser_view) {
        browser_view_ = browser_view;
    }

private:
    CefRefPtr<CefBrowserView> browser_view_;
    
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
    IMPLEMENT_REFCOUNTING(SimpleCEFWindowDelegate);
#pragma clang diagnostic pop
};

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
    }

    // CefDisplayHandler methods
    virtual void OnTitleChange(CefRefPtr<CefBrowser> browser,
                             const CefString& title) override {
        (void)browser;
        (void)title;
    }

    // CefLifeSpanHandler methods
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        (void)browser;
        std::cout << "🌐 CEF Browser created successfully" << std::endl;
    }

    virtual bool DoClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser;
        return false;
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser;
    }

private:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
    IMPLEMENT_REFCOUNTING(SimpleCEFClient);
#pragma clang diagnostic pop
};

class SimpleCEFApp : public CefApp, public CefBrowserProcessHandler {
public:
    SimpleCEFApp() = default;
    
    // CefApp methods
    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    
    // CefBrowserProcessHandler methods
    virtual void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override {
        
        (void)process_type; // Suppress unused parameter warning
        
        // Minimal keychain-disabling switches only
        command_line->AppendSwitch("--use-mock-keychain");
        command_line->AppendSwitch("--disable-password-manager");
        command_line->AppendSwitchWithValue("--password-store", "basic");
        
        // Fix macOS GPU and network service crashes
        command_line->AppendSwitch("--disable-gpu");
        command_line->AppendSwitch("--disable-gpu-compositing");
        command_line->AppendSwitch("--disable-gpu-sandbox");
        
        // Remove browser UI elements for clean application window
        command_line->AppendSwitch("--disable-features=TabStrip,Toolbar,LocationBar,Bookmarks,MenuBar");
        command_line->AppendSwitch("--hide-scrollbars");
        command_line->AppendSwitch("--disable-default-apps");
        command_line->AppendSwitch("--disable-extensions");
        command_line->AppendSwitch("--disable-plugins-discovery");
        
        // Enable Views framework for chromeless windows
        command_line->AppendSwitch("--use-views");
    }

private:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
    IMPLEMENT_REFCOUNTING(SimpleCEFApp);
#pragma clang diagnostic pop
};

inline auto openInCEF(std::string_view port, std::string_view file) -> int {
    // Minimal keychain blocking environment variables
    setenv("USE_MOCK_KEYCHAIN", "1", 1);
    setenv("DISABLE_KEYCHAIN_ACCESS", "1", 1);
    
    // CEF command line arguments
    CefMainArgs main_args;
    
    // Minimal CEF settings - let CEF use its defaults for JavaScript/web features
    CefSettings settings;
    settings.no_sandbox = true;

#ifdef __APPLE__
    // macOS-specific CEF settings
    settings.multi_threaded_message_loop = false;
    
    // Set framework and resource paths for macOS
    std::filesystem::path exe_dir = std::filesystem::current_path();
    std::filesystem::path framework_path = exe_dir / "Frameworks" / "Chromium Embedded Framework.framework";
    std::filesystem::path resources_path = framework_path / "Resources";
    std::filesystem::path exe_path = exe_dir / "src" / "WebFrontApp";
    std::filesystem::path cache_path = exe_dir / "cef_cache";
    std::filesystem::create_directories(cache_path);
    
    // Convert to absolute paths
    framework_path = std::filesystem::absolute(framework_path);
    resources_path = std::filesystem::absolute(resources_path);
    exe_path = std::filesystem::absolute(exe_path);
    cache_path = std::filesystem::absolute(cache_path);
    
    CefString(&settings.framework_dir_path).FromString(framework_path.string());
    CefString(&settings.resources_dir_path).FromString(resources_path.string());
    CefString(&settings.locales_dir_path).FromString(resources_path.string());
    CefString(&settings.browser_subprocess_path).FromString(exe_path.string());
    CefString(&settings.root_cache_path).FromString(cache_path.string());
    CefString(&settings.cache_path).FromString(cache_path.string());
    
    // Enable logging to debug macOS issues
    std::filesystem::path log_path = exe_dir / "cef_debug.log";
    CefString(&settings.log_file).FromString(log_path.string());
    settings.log_severity = LOGSEVERITY_INFO;
    
    // Note: Limited settings available in this CEF version
#endif

    // Initialize CEF with minimal custom app
    std::cout << "Attempting CEF initialization..." << std::endl;
    
    CefRefPtr<SimpleCEFApp> app(new SimpleCEFApp);
    
    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::cout << "CEF initialization failed" << std::endl;
        return -1;
    }
    std::cout << "CEF initialized successfully" << std::endl;

    // Create the browser client
    CefRefPtr<SimpleCEFClient> client(new SimpleCEFClient);

    // Build the URL
    std::string url = "http://localhost:" + std::string(port) + "/" + std::string(file);
    
    std::cout << "Opening URL: " << url << std::endl;
    
    // Give the server a moment to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Browser settings for the modern Views approach
    CefBrowserSettings browser_settings;
    
    // Create a browser view using the modern Views API - this creates a chromeless browser
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        client, url, browser_settings, nullptr, nullptr, nullptr);
    
    // Create a window delegate for controlling the window appearance
    CefRefPtr<SimpleCEFWindowDelegate> window_delegate(new SimpleCEFWindowDelegate);
    window_delegate->SetBrowserView(browser_view);
    
    // Create a top-level window with no browser chrome
    CefRefPtr<CefWindow> window = CefWindow::CreateTopLevelWindow(window_delegate);
    
    // Run the CEF message loop
    CefRunMessageLoop();

    // Shutdown CEF
    CefShutdown();

    return 0;
}

#else

inline auto openInCEF(std::string_view port, std::string_view file) -> int {
    // CEF not available, return error code
    (void)port;  // Suppress unused parameter warning
    (void)file;  // Suppress unused parameter warning
    return -1;
}

#endif // WEBFRONT_HAS_CEF