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
// Additional Windows APIs for window management
#include <windows.h>
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

// Ultra-minimal CEF client for content-only display
class ContentOnlyCEFClient : public CefClient, public CefLifeSpanHandler, public CefKeyboardHandler {
public:
    ContentOnlyCEFClient() = default;

    // CefClient methods - absolute minimum required
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    virtual CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }    // CefLifeSpanHandler methods
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        (void)browser; // Mark as used to suppress warning
        browser_ = browser;
        browser_count_++;
    }

    virtual bool DoClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser; // Mark as used to suppress warning
        return false; // Allow normal close
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        (void)browser; // Mark as used to suppress warning
        browser_count_--;
        
        if (browser_ && browser_->IsSame(browser)) {
            browser_ = nullptr;
        }
        
        // Quit when last browser closes
        if (browser_count_ == 0) {
            CefQuitMessageLoop();
        }
    }

    // CefKeyboardHandler methods - enable basic window management
    virtual bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                             const CefKeyEvent& event,
                             CefEventHandle os_event,
                             bool* is_keyboard_shortcut) override {
        (void)browser; // Mark as used to suppress warning
        (void)os_event; // Mark as used to suppress warning
        (void)is_keyboard_shortcut; // Mark as used to suppress warning
        
        // Alt+F4 to close window
        if (event.type == KEYEVENT_KEYDOWN && 
            (event.modifiers & EVENTFLAG_ALT_DOWN) && 
            event.windows_key_code == VK_F4) {
            browser->GetHost()->CloseBrowser(false);
            return true;
        }
        return false;
    }

    void CloseBrowser() {
        if (browser_) {
            browser_->GetHost()->CloseBrowser(false);
        }
    }

private:
    CefRefPtr<CefBrowser> browser_;
    std::atomic<int> browser_count_{0};
    IMPLEMENT_REFCOUNTING(ContentOnlyCEFClient);
};

// Helper function to create a minimal parent window with optional close button
#ifdef _WIN32
HWND CreateMinimalWindow(int width, int height, bool show_close_button = true) {
    // Register window class
    static bool class_registered = false;
    const wchar_t* class_name = L"WebFrontMinimal";
    
    if (!class_registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
                case WM_CLOSE:
                    PostQuitMessage(0);
                    return 0;
                case WM_DESTROY:
                    PostQuitMessage(0);
                    return 0;
                case WM_KEYDOWN:
                    // Alt+F4 to close
                    if (wParam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) {
                        PostQuitMessage(0);
                        return 0;
                    }
                    break;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        };
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = class_name;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        
        if (!RegisterClassW(&wc)) {
            return nullptr;
        }
        class_registered = true;
    }
      // Get screen dimensions to center the window
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int x = (screen_width - width) / 2;
    int y = (screen_height - height) / 2;
    
    // Create window with optional close button
    DWORD window_style, extended_style;
    
    if (show_close_button) {
        // Minimal window with close button but no other chrome
        window_style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        extended_style = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
    } else {
        // Completely frameless window
        window_style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        extended_style = WS_EX_APPWINDOW;
    }
    
    HWND hwnd = CreateWindowExW(
        extended_style,                     // Extended style
        class_name,                         // Class name
        L"WebFront",                        // Window title
        window_style,                       // Style
        x, y, width, height,                // Position and size
        nullptr,                            // Parent
        nullptr,                            // Menu
        GetModuleHandle(nullptr),          // Instance
        nullptr                             // Additional data
    );
    
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    
    return hwnd;
}
#endif

// Content-only CEF app for minimal browser windows with optional window chrome
class ContentOnlyCEFApp : public CefApp, public CefBrowserProcessHandler {
public:
    ContentOnlyCEFApp(const std::string& url, bool show_close_button = true) 
        : url_(url), show_close_button_(show_close_button) {}

    // CefApp methods
    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    // CefBrowserProcessHandler methods
    virtual void OnContextInitialized() override {        // Create the content-only browser client
        CefRefPtr<ContentOnlyCEFClient> client(new ContentOnlyCEFClient());
        
        // Ultra-minimal browser settings
        CefBrowserSettings browser_settings;
        // Only use settings that are available in this CEF version
        // Most settings are disabled by default for minimal footprint
        
        // Window configuration for truly chromeless embedded browser
        CefWindowInfo window_info;
        
#ifdef _WIN32
        int window_width = 1200;
        int window_height = 800;
        
        // Create our own minimal parent window with optional close button
        HWND parent_window = CreateMinimalWindow(window_width, window_height, show_close_button_);
        if (!parent_window) {
            std::cerr << "Failed to create parent window" << std::endl;
            return;
        }
        
        // Embed CEF browser as child window (no browser chrome)
        CefRect rect(0, 0, window_width, window_height);
        window_info.SetAsChild(parent_window, rect);
        
#elif __APPLE__
        // macOS content-only window
        CefRect rect(0, 0, 1200, 800);
        window_info.SetAsChild(0, rect);
        
#elif __linux__
        // Linux content-only window
        CefRect rect(0, 0, 1200, 800);
        window_info.SetAsChild(0, rect);
#endif

        // Create browser with chromeless settings
        CefBrowserHost::CreateBrowser(window_info, client, url_, browser_settings, nullptr, nullptr);
    }

private:
    std::string url_;
    bool show_close_button_;
    IMPLEMENT_REFCOUNTING(ContentOnlyCEFApp);
};

/// Open the web UI in a content-only CEF browser window with optional window chrome
auto openInCEF(std::string_view port, std::string_view file, bool show_close_button = true) -> int {
    auto url = std::string("http://localhost:").append(port) + std::string("/").append(file);

    std::cout << "Starting content-only CEF browser for: " << url << std::endl;

#ifdef _WIN32
    // CEF main args
    CefMainArgs main_args(GetModuleHandle(nullptr));    // Ultra-minimal CEF settings for chromeless display
    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = false;
    settings.log_severity = LOGSEVERITY_FATAL; // Absolutely minimal logging
    
    // Disable all unnecessary CEF features
    settings.remote_debugging_port = -1; // No debugging
    settings.background_color = 0xFFFFFFFF; // White background
    
    // Reduce resource usage
    settings.uncaught_exception_stack_size = 0;
      // Create content-only app with optional close button
    CefRefPtr<ContentOnlyCEFApp> app(new ContentOnlyCEFApp(url, show_close_button));

    // Initialize CEF with minimal configuration
    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        std::cout << "Failed to initialize CEF, falling back to default browser..." << std::endl;
        return openInDefaultBrowser(port, file);
    }

    if (show_close_button) {
        std::cout << "CEF browser window created with close button (no address bar, minimal UI)" << std::endl;
    } else {
        std::cout << "Chromeless CEF browser window created (no address bar, no window UI)" << std::endl;
    }
    std::cout << "Press Alt+F4 to close the window" << std::endl;

    // Run message loop
    CefRunMessageLoop();

    // Clean shutdown
    CefShutdown();
    return 0;

#else
    // For non-Windows platforms, use default browser for now
    std::cout << "Content-only CEF implementation currently Windows-only, using default browser..." << std::endl;
    return openInDefaultBrowser(port, file);
#endif
}

} // namespace webfront
