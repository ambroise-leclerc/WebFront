#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include <tooling/Logger.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#elif __linux__
#include <fstream>
#elif __APPLE__
#include <crt_externs.h>
#endif

#ifdef WEBFRONT_EMBED_CEF

#include "cef_app.h"
#include "cef_browser.h"
#include "cef_browser_process_handler.h"
#include "cef_client.h"
#include "cef_command_line.h"
#include "views/cef_browser_view.h"
#include "views/cef_window.h"
#include "views/cef_window_delegate.h"
#include "wrapper/cef_library_loader.h"

namespace webfront::cef {

// Platform-specific function to get command line arguments
std::pair<int, std::vector<std::string>> getCommandLineArgs() {
#ifdef _WIN32
    LPWSTR* argv_w;
    int argc;
    argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) {
        int size = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), size, nullptr, nullptr);
        argv.push_back(std::move(arg));
    }
    
    LocalFree(argv_w);
    return {argc, std::move(argv)};
    
#elif __linux__
    std::ifstream cmdline("/proc/self/cmdline");
    std::vector<std::string> argv;
    std::string arg;
    
    while (std::getline(cmdline, arg, '\0')) {
        if (!arg.empty()) {
            argv.push_back(arg);
        }
    }
    
    return {static_cast<int>(argv.size()), std::move(argv)};
    
#elif __APPLE__
    char*** argv_ptr = _NSGetArgv();
    int* argc_ptr = _NSGetArgc();
    
    if (!argv_ptr || !argc_ptr || !*argv_ptr) {
        return {0, {}};
    }
    
    int argc = *argc_ptr;
    char** argv_c = *argv_ptr;
    std::vector<std::string> argv;
    
    for (int i = 0; i < argc; ++i) {
        argv.emplace_back(argv_c[i]);
    }
    
    return {argc, std::move(argv)};
    
#else
    // Fallback - return empty arguments
    return {0, {}};
#endif
}

// Initialize CEF and handle subprocesses
// Returns: -1 on error, 0 to continue with main process, >0 if this is a subprocess (should exit)
int initialize() {
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
        std::cout << "Failed to load CEF library" << std::endl;
        return -1;
    }
    
    // Get command line arguments using platform-specific APIs
    auto [argc, argv_vec] = getCommandLineArgs();
    
    // Convert std::vector<std::string> to char** for CEF
    std::vector<char*> argv_ptrs;
    for (auto& arg : argv_vec) {
        argv_ptrs.push_back(arg.data());
    }
    
    // CEF subprocesses require CefExecuteProcess to be called first
    CefMainArgs main_args(argc, argv_ptrs.data());
    
    // Create a simple app for subprocess handling
    CefRefPtr<CefApp> app;
    
    // Execute the subprocess if this is a helper process
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        // If this is a subprocess, return the exit code (caller should exit)
        return exit_code;
    }
    
    // Continue with main process
    return 0;
#else
    return 0;
#endif
}

class SimpleCEFWindowDelegate : public CefWindowDelegate {
public:
    SimpleCEFWindowDelegate() = default;

    // CefWindowDelegate methods
    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        // Add the browser view to the window
        window->AddChildView(browserView);

        // Set window properties for a clean application window
        window->SetTitle("WebFront Application");
        window->CenterWindow(CefSize(1200, 800));
        window->Show();
    }

    void OnWindowDestroyed(CefRefPtr<CefWindow> /*window*/) override {
        browserView = nullptr;
        // Quit the CEF message loop when the window is destroyed
        CefQuitMessageLoop();
    }

    bool CanClose(CefRefPtr<CefWindow> /*window*/) override {
        // Allow the window to close, which will terminate the app
        return true;
    }

    CefSize GetPreferredSize(CefRefPtr<CefView> /*view*/) override {
        return CefSize(1200, 800);
    }

    void SetBrowserView(CefRefPtr<CefBrowserView> view) { browserView = view; }

private:
    CefRefPtr<CefBrowserView> browserView;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
    IMPLEMENT_REFCOUNTING(SimpleCEFWindowDelegate);
#pragma clang diagnostic pop
};

class SimpleCEFClient : public CefClient, public CefDisplayHandler, public CefLifeSpanHandler {
public:
    SimpleCEFClient() = default;
    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    virtual void OnTitleChange(CefRefPtr<CefBrowser> /*browser*/, const CefString& /*title*/) override {}
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> /*browser*/) override {}
    virtual bool DoClose(CefRefPtr<CefBrowser> /*browser*/) override { return false; }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) override { 
        // Ensure message loop exits when browser closes
        CefQuitMessageLoop();
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
    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    // CefBrowserProcessHandler methods
    virtual void OnBeforeCommandLineProcessing(const CefString& /*process_type*/, CefRefPtr<CefCommandLine> command_line) override {
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

void open(std::string_view port, std::string_view file) {
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
        throw std::runtime_error("CEF initialization failed");
    }
    
    // Create the browser client
    CefRefPtr<SimpleCEFClient> client(new SimpleCEFClient);

    // Build the URL
    std::string url = "http://localhost:" + std::string(port) + "/" + std::string(file);

    log::info("Opening URL: {}", url);

    // Give the server a moment to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Browser settings for the modern Views approach
    CefBrowserSettings browser_settings;

    // Create a browser view using the modern Views API - this creates a chromeless browser
    CefRefPtr<CefBrowserView> browser_view =
      CefBrowserView::CreateBrowserView(client, url, browser_settings, nullptr, nullptr, nullptr);

    // Create a window delegate for controlling the window appearance
    CefRefPtr<SimpleCEFWindowDelegate> window_delegate(new SimpleCEFWindowDelegate);
    window_delegate->SetBrowserView(browser_view);

    // Create a top-level window with no browser chrome
    CefRefPtr<CefWindow> window = CefWindow::CreateTopLevelWindow(window_delegate);

    // Run the CEF message loop
    CefRunMessageLoop();

    // Shutdown CEF
    CefShutdown();
}
static constexpr bool webfrontEmbedCEF{true};
} // namespace webfront::cef
#else
namespace webfront::cef {

void open(std::string_view /*port*/, std::string_view /*file*/) {
    throw std::runtime_error("cef::open() : CEF not available");
}
static constexpr bool webfrontEmbedCEF{false};
} // namespace webfront::cef
#endif // WEBFRONT_EMBED_CEF