#pragma once

#include "../ContentPolicy.hpp"
#include "../system/WindowsCompat.hpp"
#include "../tooling/Logger.hpp"
#include "CEFPaths.hpp"
#include "Capture.hpp"
#include "DefaultBrowser.hpp"
#include "Url.hpp"
#include "Window.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
    #include <shellapi.h>
#elif __linux__
    #include <fstream>
#elif __APPLE__
    #include <crt_externs.h>
    #include <mach-o/dyld.h>
#endif

namespace webfront::cef {

// Exception types for CEF initialization
class CEFInitializationError : public std::runtime_error {
public:
    explicit CEFInitializationError(const std::string& message) : std::runtime_error("CEF initialization failed: " + message) { }
};

class CEFSubprocessExit : public std::exception {
    int exit_code_;

public:
    explicit CEFSubprocessExit(int exit_code) : exit_code_(exit_code) { }

    [[nodiscard]] const char* what() const noexcept override {
        return "CEF subprocess should exit";
    }

    [[nodiscard]] int exit_code() const noexcept {
        return exit_code_;
    }
};

}  // namespace webfront::cef

#ifdef WEBFRONT_EMBED_CEF

    #ifndef _WIN32
        #include <csignal>
    #endif

    // CEF defines macros such as NO_UNIQUE_ADDRESS and ALWAYS_INLINE that break stdexec's preprocessor
    // machinery when it is included afterwards: pull every stdnet and stdexec header WebFront uses in first.
    #include "../networking/SenderHeaders.hpp"

    #include "cef_app.h"
    #include "cef_browser.h"
    #include "cef_browser_process_handler.h"
    #include "cef_client.h"
    #include "cef_command_handler.h"
    #include "cef_command_line.h"
    #include "cef_context_menu_handler.h"
    #include "cef_devtools_message_observer.h"
    #include "cef_frame.h"
    #include "cef_id_mappers.h"
    #include "cef_load_handler.h"
    #include "cef_parser.h"
    #include "cef_task.h"
    #include "cef_request_handler.h"
    #include "views/cef_browser_view.h"
    #include "views/cef_window.h"
    #include "views/cef_window_delegate.h"
    #ifdef __APPLE__
        #include "wrapper/cef_library_loader.h"
    #endif

namespace webfront::cef {
// Compile-time constant indicating CEF availability
static constexpr bool webfrontEmbedCEF{true};

// Constants
namespace {
constexpr const char* kCEFCacheDir         = "cef_cache";
constexpr const char* kCEFLogFile          = "cef_debug.log";
constexpr const char* kICUDataFile         = "icudtl.dat";
constexpr const char* kLocalesDir          = "locales";
constexpr const char* kResourcesDir        = "Resources";
}  // namespace

// Helper function to set keychain-related environment variables
inline void setKeychainEnvironment() {
    #if defined(__APPLE__) || defined(__linux__)
    setenv("DISABLE_KEYCHAIN_ACCESS", "1", 1);
    setenv("OSX_DISABLE_KEYCHAIN", "1", 1);
    setenv("USE_MOCK_KEYCHAIN", "1", 1);
    setenv("CHROME_KEYCHAIN_REAUTH_DISABLED", "1", 1);
    setenv("PASSWORD_MANAGER_ENABLED", "0", 1);
    #endif
}

    #ifdef __APPLE__
// Resolves the actual running executable's path, independent of its target name or working
// directory. CEF needs this for the browser subprocess and to locate the Frameworks/ directory
// deployed by CMake in the parent of the executable's directory, independent of the target name.
inline std::filesystem::path currentExecutablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        throw CEFInitializationError("Unable to resolve the current executable path");
    return std::filesystem::canonical(buffer);
}
    #endif

// Helper function to configure platform-specific paths
inline void configurePlatformPaths(CefSettings& settings [[maybe_unused]]) {
    #ifdef __APPLE__
    // Set framework and resource paths for macOS
    std::filesystem::path exe_path       = currentExecutablePath();
    std::filesystem::path exe_dir        = exe_path.parent_path();
    std::filesystem::path framework_path = detail::macOSFrameworkPath(exe_path);
    std::filesystem::path resources_path = framework_path / kResourcesDir;
    std::filesystem::path cache_path     = exe_dir / kCEFCacheDir;
    std::filesystem::create_directories(cache_path);

    // Convert to absolute paths
    framework_path = std::filesystem::absolute(framework_path);
    resources_path = std::filesystem::absolute(resources_path);
    exe_path       = std::filesystem::absolute(exe_path);
    cache_path     = std::filesystem::absolute(cache_path);

    CefString(&settings.framework_dir_path).FromString(framework_path.string());
    CefString(&settings.resources_dir_path).FromString(resources_path.string());
    CefString(&settings.locales_dir_path).FromString(resources_path.string());
    CefString(&settings.browser_subprocess_path).FromString(exe_path.string());
    CefString(&settings.root_cache_path).FromString(cache_path.string());
    CefString(&settings.cache_path).FromString(cache_path.string());

    // Enable logging
    std::filesystem::path log_path = exe_dir / kCEFLogFile;
    CefString(&settings.log_file).FromString(log_path.string());
    #elif defined(__linux__)
    // Set resource paths for Linux
    std::filesystem::path current_exe = std::filesystem::canonical("/proc/self/exe");
    std::filesystem::path exe_dir     = current_exe.parent_path();

    std::filesystem::path resources_path = exe_dir;
    std::filesystem::path locales_path   = resources_path / kLocalesDir;
    std::filesystem::path exe_path       = current_exe;
    std::filesystem::path cache_path     = exe_dir / kCEFCacheDir;
    std::filesystem::create_directories(cache_path);

    // Convert to absolute paths
    resources_path = std::filesystem::absolute(resources_path);
    locales_path   = std::filesystem::absolute(locales_path);
    exe_path       = std::filesystem::absolute(exe_path);
    cache_path     = std::filesystem::absolute(cache_path);

    log::info("CEF Linux paths:");
    log::info("  Resources: {}", resources_path.string());
    log::info("  Locales: {}", locales_path.string());
    log::info("  Executable: {}", exe_path.string());
    log::info("  Cache: {}", cache_path.string());

    // Check if ICU data file exists
    std::filesystem::path icu_file = resources_path / kICUDataFile;
    log::info("  ICU data file: {} (exists: {})", icu_file.string(), std::filesystem::exists(icu_file));

    CefString(&settings.resources_dir_path).FromString(resources_path.string());
    CefString(&settings.locales_dir_path).FromString(locales_path.string());
    CefString(&settings.browser_subprocess_path).FromString(exe_path.string());
    CefString(&settings.root_cache_path).FromString(cache_path.string());
    CefString(&settings.cache_path).FromString(cache_path.string());

    // Enable logging
    std::filesystem::path log_path = exe_dir / kCEFLogFile;
    CefString(&settings.log_file).FromString(log_path.string());
    #endif
}

// Platform-specific function to get command line arguments (inline definition)
inline std::pair<int, std::vector<std::string>> getCommandLineArgs() {
    #if defined(_WIN32)
    LPWSTR* argv_w;
    int     argc;
    argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) {
        int         size = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, arg.data(), size, nullptr, nullptr);
        argv.push_back(std::move(arg));
    }
    LocalFree(argv_w);
    return {argc, std::move(argv)};
    #elif defined(__linux__)
    std::ifstream            cmdline("/proc/self/cmdline");
    std::vector<std::string> argv;
    std::string              arg;
    while (std::getline(cmdline, arg, '\0')) {
        if (!arg.empty())
            argv.push_back(arg);
    }
    return {static_cast<int>(argv.size()), std::move(argv)};
    #elif defined(__APPLE__)
    char*** argv_ptr = _NSGetArgv();
    int*    argc_ptr = _NSGetArgc();
    if (!argv_ptr || !argc_ptr || !*argv_ptr) {
        return {0, {}};
    }
    int                      argc   = *argc_ptr;
    char**                   argv_c = *argv_ptr;
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) {
        argv.emplace_back(argv_c[i]);
    }
    return {argc, std::move(argv)};
    #else
    return {0, {}};
    #endif
}

// Helper to build CefMainArgs portably
inline CefMainArgs makeMainArgs(
    #if !defined(_WIN32)
    int    argc,
    char** argv
    #endif
) {
    #if defined(_WIN32)
    return CefMainArgs(GetModuleHandle(nullptr));
    #else
    return CefMainArgs(argc, argv);
    #endif
}

// Initialize CEF and handle subprocesses
// Throws: CEFInitializationError on initialization failure
// Throws: CEFSubprocessExit if this is a subprocess (caller should exit with the provided code)
// Returns: void on successful main process initialization
inline void initialize() {
    if constexpr (!webfrontEmbedCEF) {
        return;
    }
    // Set environment variables BEFORE any CEF operations
    setKeychainEnvironment();
    #ifdef __APPLE__
    // Load the CEF framework library at runtime - required on macOS
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) {
        throw CEFInitializationError("Failed to load CEF framework library");
    }
    #endif
    auto [argc, argv_vec] = getCommandLineArgs();
    std::vector<char*> argv_ptrs;
    for (auto& arg : argv_vec) {
        argv_ptrs.push_back(arg.data());
    }
    CefMainArgs main_args = makeMainArgs(
    #if !defined(_WIN32)
        argc,
        argv_ptrs.data()
    #endif
    );
    CefRefPtr<CefApp> app;
    int               exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0) {
        throw CEFSubprocessExit(exit_code);
    }
    // Main process continues - initialization successful
}

class SimpleCEFWindowDelegate : public CefWindowDelegate {
public:
    explicit SimpleCEFWindowDelegate(WindowOptions options = frontend::detail::resolveWindow({})) : windowOptions(std::move(options)) {}

    // CefWindowDelegate methods
    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        // Add the browser view to the window
        window->AddChildView(browserView);

        // Set window properties for a clean application window
        window->SetTitle(windowOptions.title);
        window->CenterWindow(CefSize(windowOptions.width, windowOptions.height));
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
        return CefSize(windowOptions.width, windowOptions.height);
    }

    void SetBrowserView(CefRefPtr<CefBrowserView> view) {
        browserView = view;
    }

private:
    CefRefPtr<CefBrowserView> browserView;
    WindowOptions             windowOptions;

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(SimpleCEFWindowDelegate);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};

namespace detail {
// DevTools must not inherit the application's navigation policy or quit its message loop.
class DevToolsWindowClient : public CefClient {
private:
    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(DevToolsWindowClient);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};
}  // namespace detail

using frontend::CaptureProvider;
using frontend::CaptureRegistration;
using frontend::PendingCaptures;
using frontend::CaptureResult;

namespace detail {
/// The browser as seen by capture requests: read and cleared only on the UI thread, so a request that outlives the
/// window finds it null.
struct CaptureJob;
struct CaptureTarget {
    CefRefPtr<CefBrowser>                    browser;
    PendingCaptures<CaptureJob> pending;  // Requests nobody has answered yet, queued or in flight.
};

struct CaptureJob {
    std::shared_ptr<CaptureTarget> target;
    std::promise<CaptureResult>    done;
};

inline void answer(CaptureJob& job, CaptureResult result) noexcept {
    try {
        job.done.set_value(std::move(result));
    } catch (...) { }
}

/// The window is going away: no pending request waits for an answer that will never come.
inline void abandonCaptures(CaptureTarget& target) noexcept {
    target.browser = nullptr;
    for (const auto& job : target.pending.close())
        answer(*job, std::unexpected("the window is closed"));
}


// The PNG of a Page.captureScreenshot result: {"data": "<base64>"}.
inline CaptureResult pngOf(const void* json, std::size_t size) {
    const auto value = CefParseJSON(json, size, JSON_PARSER_RFC);
    if (!value || value->GetType() != VTYPE_DICTIONARY)
        return std::unexpected("the engine returned an unreadable capture");
    const auto data = CefBase64Decode(value->GetDictionary()->GetString("data"));
    if (!data || !data->GetSize())
        return std::unexpected("the engine returned no image");
    std::vector<std::uint8_t> png(data->GetSize());
    data->GetData(png.data(), png.size(), 0);
    return png;
}

// Answers one Page.captureScreenshot. It stays registered with the browser until that answer arrives, and drops its own
// registration then; the job outlives a waiter that timed out.
class CaptureObserver : public CefDevToolsMessageObserver {
public:
    explicit CaptureObserver(std::shared_ptr<CaptureJob> request) : job(std::move(request)) { }

    void start(CefRefPtr<CefBrowserHost> host) {
        registration = host->AddDevToolsMessageObserver(this);
        auto params  = CefDictionaryValue::Create();
        params->SetString("format", "png");
        expected = host->ExecuteDevToolsMethod(0, "Page.captureScreenshot", params);
        if (!expected) {
            answer(*job, std::unexpected("the engine refused the capture request"));
            registration = nullptr;
            return;
        }
    }

    void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int messageId, bool success, const void* result, size_t size) override {
        if (messageId != expected || !job)
            return;
        job->target->pending.untrack(job);
        if (success)
            answer(*job, pngOf(result, size));
        else
            answer(*job, std::unexpected("the engine failed the capture: " + std::string(static_cast<const char*>(result), size)));
        job          = nullptr;
        registration = nullptr;  // Last: it may destroy this observer.
    }

private:
    std::shared_ptr<CaptureJob> job;
    CefRefPtr<CefRegistration>  registration;
    int                         expected = 0;

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(CaptureObserver);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};

class CaptureTask : public CefTask {
public:
    explicit CaptureTask(std::shared_ptr<CaptureJob> request) : job(std::move(request)) { }
    void Execute() override {
        try {
            if (!job->target->browser) {
                answer(*job, std::unexpected("the window is closed"));
                return;
            }
            CefRefPtr<CaptureObserver> observer = new CaptureObserver(job);
            observer->start(job->target->browser->GetHost());
        } catch (...) {
            try {
                job->done.set_exception(std::current_exception());
            } catch (...) { }
        }
    }

private:
    std::shared_ptr<CaptureJob> job;

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(CaptureTask);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};

inline CaptureProvider captureProvider(std::shared_ptr<CaptureTarget> target) {
    return [target = std::move(target)](std::chrono::milliseconds timeout) -> CaptureResult {
        auto job    = std::make_shared<CaptureJob>(CaptureJob{target, {}});
        auto future = job->done.get_future();
        if (!target->pending.track(job))
            return std::unexpected("the window is closed");
        if (!CefPostTask(TID_UI, new CaptureTask(job))) {
            target->pending.untrack(job);
            return std::unexpected("the window is closed");
        }
        if (future.wait_for(timeout) != std::future_status::ready) {
            target->pending.untrack(job);
            return std::unexpected("the window did not answer within the capture timeout");
        }
        return future.get();
    };
}
}  // namespace detail

class SimpleCEFClient : public CefClient,
                        public CefDisplayHandler,
                        public CefLifeSpanHandler,
                        public CefLoadHandler,
                        public CefRequestHandler,
                        public CefContextMenuHandler,
                        public CefCommandHandler {
public:
    explicit SimpleCEFClient(webfront::detail::BoundContentPolicy policy = webfront::detail::BoundContentPolicy{}, browser::Launcher launch = browser::launch)
        : contentPolicy(std::move(policy)), popupLauncher(launch) { }

    void setContentPolicy(webfront::detail::BoundContentPolicy policy) {
        contentPolicy = std::move(policy);
    }

    CefRefPtr<CefRequestHandler> GetRequestHandler() override {
        return this;
    }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
        return this;
    }
    CefRefPtr<CefCommandHandler> GetCommandHandler() override {
        return this;
    }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool, bool) override {
        // Frame navigations and subresources remain outside this top-level navigation policy.
        return frame->IsMain() && !contentPolicy.allowsNavigation(request->GetURL().ToString());
    }

    bool
    OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, const CefString& url, cef_window_open_disposition_t disposition, bool userGesture) override {
        if (disposition == CEF_WOD_CURRENT_TAB)
            return !contentPolicy.allowsNavigation(url.ToString());
        dispatchPopup(url.ToString(), userGesture);
        return true;
    }

    bool OnBeforePopup(CefRefPtr<CefBrowser>,
                       CefRefPtr<CefFrame>,
                       int,
                       const CefString& url,
                       const CefString&,
                       cef_window_open_disposition_t,
                       bool userGesture,
                       const CefPopupFeatures&,
                       CefWindowInfo&,
                       CefRefPtr<CefClient>&,
                       CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&,
                       bool*) override {
        dispatchPopup(url.ToString(), userGesture);
        return true;
    }

    void OnBeforeDevToolsPopup(CefRefPtr<CefBrowser>, CefWindowInfo&, CefRefPtr<CefClient>& client, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*)
        override {
        client = new detail::DevToolsWindowClient;
    }

    bool OnChromeCommand(CefRefPtr<CefBrowser>, int command, cef_window_open_disposition_t) override {
        if (contentPolicy.options().devToolsEnabled)
            return false;
        for (const auto* name : {"IDC_DEV_TOOLS",
                                 "IDC_DEV_TOOLS_CONSOLE",
                                 "IDC_DEV_TOOLS_DEVICES",
                                 "IDC_DEV_TOOLS_INSPECT",
                                 "IDC_DEV_TOOLS_TOGGLE",
                                 "IDC_CONTENT_CONTEXT_INSPECTELEMENT",
                                 "IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_DEVTOOLS"})
            if (command == cef_id_for_command_id_name(name))
                return true;
        return false;
    }

    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel> menu) override {
        if (!contentPolicy.options().contextMenusEnabled)
            menu->Clear();
        else if (!contentPolicy.options().devToolsEnabled) {
            menu->Remove(cef_id_for_command_id_name("IDC_CONTENT_CONTEXT_INSPECTELEMENT"));
            menu->Remove(cef_id_for_command_id_name("IDC_CONTENT_CONTEXT_INSPECTELEMENT_WITH_DEVTOOLS"));
        }
    }

    // Read after the message loop and shutdown: callbacks report failures without throwing
    // through CEF. Once the first page has loaded, later navigation failures stay in the UI.
    std::string navigationFailure;

    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        // The first completed main-frame load deliberately ends startup error tracking.
        if (frame->IsMain())
            loadingInitialPage = false;
    }

    void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode error, const CefString& errorText, const CefString&) override {
        // Closing a window or replacing a navigation may abort a request normally.
        if (!loadingInitialPage || !frame->IsMain() || error == ERR_ABORTED)
            return;
        navigationFailure = "CEF initial navigation failed: " + errorText.ToString();
        browser->GetHost()->CloseBrowser(true);
    }

    virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    virtual void OnTitleChange(CefRefPtr<CefBrowser> /*browser*/, const CefString& /*title*/) override { }
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        // The application's window is the first browser; popups are cancelled and DevTools has its own client.
        if (!captureTarget->browser) {
            captureTarget->browser = browser;
        captureTarget->pending.open();
            captureRegistration    = CaptureRegistration{detail::captureProvider(captureTarget)};
        }
    }
    virtual bool DoClose(CefRefPtr<CefBrowser> /*browser*/) override {
        return false;
    }

    virtual void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (captureTarget->browser && captureTarget->browser->IsSame(browser)) {
            captureRegistration = {};
            detail::abandonCaptures(*captureTarget);
        }
        // Ensure message loop exits when browser closes
        CefQuitMessageLoop();
    }

private:
    webfront::detail::BoundContentPolicy contentPolicy;
    browser::Launcher                    popupLauncher;
    bool loadingInitialPage = true;
    std::shared_ptr<detail::CaptureTarget> captureTarget = std::make_shared<detail::CaptureTarget>();
    CaptureRegistration                    captureRegistration;

    void dispatchPopup(const std::string& url, bool userGesture) noexcept {
        try {
            contentPolicy.handlePopup(url, userGesture, popupLauncher);
        } catch (const std::exception& error) {
            log::error("CEF popup launch failed: {}", error.what());
        }
    }

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(SimpleCEFClient);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};

class SimpleCEFApp : public CefApp, public CefBrowserProcessHandler {
public:
    SimpleCEFApp() = default;

    // CefApp methods
    virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

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

        // Linux-specific fixes for CEF initialization issues
    #ifdef __linux__
        command_line->AppendSwitch("--no-sandbox");
        command_line->AppendSwitch("--disable-dev-shm-usage");
        command_line->AppendSwitch("--disable-gpu-sandbox");
        command_line->AppendSwitch("--disable-software-rasterizer");
        command_line->AppendSwitch("--disable-background-timer-throttling");
        command_line->AppendSwitch("--disable-renderer-backgrounding");
        command_line->AppendSwitch("--disable-backgrounding-occluded-windows");
    #endif

        // Enable Views framework for chromeless windows
        command_line->AppendSwitch("--use-views");
    }

private:
    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(SimpleCEFApp);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};

namespace detail {

inline void openWindow(std::string url, webfront::detail::BoundContentPolicy contentPolicy,
                       CefRefPtr<SimpleCEFClient> client = nullptr, const std::filesystem::path& profile = {},
                       WindowOptions windowSettings = {}, bool preserveSignalHandlers = false) {
    setKeychainEnvironment();
    // Build platform-appropriate CefMainArgs
    auto [argc, argv_vec] = getCommandLineArgs();
    std::vector<char*> argv_ptrs;
    for (auto& arg : argv_vec) {
        argv_ptrs.push_back(arg.data());
    }
    CefMainArgs main_args = makeMainArgs(
    #if !defined(_WIN32)
        argc,
        argv_ptrs.data()
    #endif
    );
    CefSettings settings;
    settings.no_sandbox = true;
    // An embedder owning native shutdown must keep its SIGINT/SIGTERM handlers.
    settings.disable_signal_handlers = preserveSignalHandlers;
    // Common settings for all platforms
    settings.multi_threaded_message_loop = false;
    settings.log_severity                = LOGSEVERITY_INFO;

    // Platform-specific path configuration
    configurePlatformPaths(settings);
    if (!profile.empty()) {
        const auto absoluteProfile = std::filesystem::absolute(profile);
        std::filesystem::create_directories(absoluteProfile);
        CefString(&settings.root_cache_path).FromString(absoluteProfile.string());
        CefString(&settings.cache_path).FromString(absoluteProfile.string());
        CefString(&settings.log_file).FromString((absoluteProfile / "cef_debug.log").string());
    }
    log::info("Attempting CEF initialization...");
    CefRefPtr<SimpleCEFApp> app(new SimpleCEFApp);
    #ifndef _WIN32
    struct sigaction previousInt{}, previousTerm{};
    if (preserveSignalHandlers && (::sigaction(SIGINT, nullptr, &previousInt) || ::sigaction(SIGTERM, nullptr, &previousTerm)))
        throw std::runtime_error("Cannot preserve native signal handlers");
    #endif
    const bool initialized = CefInitialize(main_args, settings, app.get(), nullptr);
    #ifndef _WIN32
    // Chrome's browser startup can install handlers independently of ContentMain's setting.
    // Restore the embedder's exact actions once all browser initialization has finished.
    if (preserveSignalHandlers && (::sigaction(SIGINT, &previousInt, nullptr) || ::sigaction(SIGTERM, &previousTerm, nullptr))) {
        if (initialized) CefShutdown();
        throw std::runtime_error("Cannot restore native signal handlers");
    }
    #endif
    if (!initialized) {
        throw std::runtime_error("CEF initialization failed");
    }
    // Create the browser client
    if (!client)
        client = new SimpleCEFClient(std::move(contentPolicy));
    else
        client->setContentPolicy(std::move(contentPolicy));

    log::info("Opening URL: {}", url);

    // Browser settings for the modern Views approach
    CefBrowserSettings browser_settings;

    // Create a browser view using the modern Views API - this creates a chromeless browser
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(client, url, browser_settings, nullptr, nullptr, nullptr);

    // Create a window delegate for controlling the window appearance
    CefRefPtr<SimpleCEFWindowDelegate> window_delegate(new SimpleCEFWindowDelegate(frontend::detail::resolveWindow(std::move(windowSettings))));
    window_delegate->SetBrowserView(browser_view);

    // Create a top-level window with no browser chrome
    CefRefPtr<CefWindow> window = CefWindow::CreateTopLevelWindow(window_delegate);

    // Run the CEF message loop
    CefRunMessageLoop();

    // Shutdown CEF
    CefShutdown();
    if (!client->navigationFailure.empty())
        throw std::runtime_error(client->navigationFailure);
}

}  // namespace detail

inline void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy = {},
                 const WindowOptions& window = {}) {
    detail::openWindow(frontend::detail::applicationUrl(address, port, file),
                       webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                       nullptr,
                       {},
                       window);
}

}  // namespace webfront::cef
#else

namespace webfront::cef {
// Compile-time constant indicating CEF availability
static constexpr bool webfrontEmbedCEF{false};

// Stub implementations for when CEF is not available. Selecting CEFFrontend without CEF support
// must fail clearly and as early as possible (initialize() runs once at BasicWF construction,
// before open() would otherwise be reached from openAndRun()).
inline void initialize() {
    throw std::runtime_error("webfront::frontend::CEFFrontend requires CEF support, but this build was configured with "
                             "WEBFRONT_EMBED_CEF=OFF. Reconfigure with -DWEBFRONT_EMBED_CEF=ON to use the embedded CEF frontend.");
}

inline void open(std::string_view /*address*/, std::string_view /*port*/, std::string_view /*file*/, const ContentPolicy& = {}, const WindowOptions& = {}) {
    throw std::runtime_error("webfront::frontend::CEFFrontend requires CEF support, but this build was configured with "
                             "WEBFRONT_EMBED_CEF=OFF. Reconfigure with -DWEBFRONT_EMBED_CEF=ON to use the embedded CEF frontend.");
}

}  // namespace webfront::cef
#endif  // WEBFRONT_EMBED_CEF

namespace webfront::cef {
inline void open(std::string_view port, std::string_view file) {
    open("127.0.0.1", port, file);
}
}  // namespace webfront::cef
