#pragma once

#include "../ContentPolicy.hpp"
#include "../tooling/Logger.hpp"
#include "Action.hpp"
#include "Capture.hpp"
#include "DefaultBrowser.hpp"
#include "Url.hpp"
#include "Window.hpp"

#include <chrono>
#include <concepts>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace webfront::frontend {

/** @brief Per-opening runtime deadline, persistent profile directory and embedded-content rules. */
struct WebView2Options {
    std::chrono::milliseconds startupTimeout{std::chrono::seconds{30}};
    std::filesystem::path     userDataFolder{};
    ContentPolicy             contentPolicy{};
    WindowOptions             window{};
    /** @brief Keep the native window hidden while retaining the real controller and message loop. */
    bool                      visible{true};
};

}  // namespace webfront::frontend

#if defined(WEBFRONT_EMBED_WEBVIEW2) && defined(_WIN32)
    #include "../system/WindowsCompat.hpp"

    #include <cstdint>
    #include <deque>
    #include <exception>
    #include <filesystem>
    #include <format>
    #include <future>
    #include <limits>
    #include <memory>
    #include <mutex>
    #include <string>
    #include <vector>

    #include <shlobj.h>
    #include <WebView2.h>
    #include <wrl.h>

namespace webfront::frontend::detail::webview2 {

using EnvironmentFactory = decltype(&CreateCoreWebView2EnvironmentWithOptions);

inline void check(HRESULT result, const char* operation) {
    if (FAILED(result))
        throw std::runtime_error(std::format("WebView2: {} failed (HRESULT 0x{:08X})", operation, static_cast<std::uint32_t>(result)));
}

class Apartment {
public:
    Apartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (result == RPC_E_CHANGED_MODE)
            throw std::runtime_error("WebView2: open() requires an STA thread; this thread already uses MTA COM");
        check(result, "STA COM initialization");
    }
    ~Apartment() {
        CoUninitialize();
    }
    Apartment(const Apartment&)            = delete;
    Apartment& operator=(const Apartment&) = delete;
};

inline std::wstring utf16(std::string_view text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("WebView2: URL is too long");
    const auto length = static_cast<int>(text.size());
    const int  size   = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!size)
        throw std::runtime_error("WebView2: invalid UTF-8 URL");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, result.data(), size))
        throw std::runtime_error("WebView2: URL conversion failed");
    return result;
}

inline std::string utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const auto length = static_cast<int>(text.size());
    const int  size   = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (!size)
        throw std::runtime_error("WebView2: invalid UTF-16 URL");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size, nullptr, nullptr))
        throw std::runtime_error("WebView2: URL conversion failed");
    return result;
}

inline void configureContentSettings(ICoreWebView2Settings* settings, const ContentPolicy& policy) {
    check(settings->put_AreDevToolsEnabled(policy.devToolsEnabled), "configuring DevTools");
    check(settings->put_AreDefaultContextMenusEnabled(policy.contextMenusEnabled), "configuring context menus");
}

inline std::wstring applicationUrl(std::string_view address, std::string_view port, std::string_view file) {
    return utf16(frontend::detail::applicationUrl(address, port, file));
}

inline std::wstring applicationUrl(std::string_view port, std::string_view file) {
    return applicationUrl("127.0.0.1", port, file);
}

inline std::wstring executablePath() {
    std::wstring executable(32768, L'\0');
    const auto   size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!size || size == executable.size())
        throw std::runtime_error("WebView2: cannot locate executable");
    executable.resize(size);
    return executable;
}

inline std::uint64_t profileIdentity(std::wstring_view executable) {
    // Stable, case-sensitive FNV-64 identity includes the full executable path.
    std::uint64_t identity = 14695981039346656037ull;
    for (const auto character : executable) {
        identity ^= static_cast<std::uint64_t>(character);
        identity *= 1099511628211ull;
    }
    return identity;
}

inline std::filesystem::path userDataFolder() {
    PWSTR raw = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw), "locating LocalAppData");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> local(raw, CoTaskMemFree);
    const auto folder = std::filesystem::path(local.get()) / L"WebFront" / std::to_wstring(profileIdentity(executablePath())) / L"WebView2";
    std::filesystem::create_directories(folder);
    return folder;
}

/// Posted to the window to make its STA start the queued capture requests.
inline constexpr UINT captureMessage = WM_APP + 0x4C1;

struct CaptureJob {
    std::promise<CaptureResult> done;
};

/**
 * Capture requests cross from any thread to the window's STA, where CapturePreview must run. The requests wait in this
 * queue, not in message payloads, so none can leak when the window disappears: closing answers every pending one.
 */
class CaptureQueue {
public:
    /// The window now accepts requests.
    void open(HWND handle) {
        std::scoped_lock lock{mutex};
        window = handle;
        opened = true;
    }

    /// Refuses further requests and returns those still waiting, to be answered by the caller.
    std::deque<std::shared_ptr<CaptureJob>> close() {
        std::scoped_lock lock{mutex};
        opened = false;
        window = nullptr;
        return std::exchange(pending, {});
    }

    bool submit(const std::shared_ptr<CaptureJob>& job) {
        std::scoped_lock lock{mutex};
        if (!opened || !PostMessageW(window, captureMessage, 0, 0))
            return false;
        pending.push_back(job);
        return true;
    }

    void cancel(const std::shared_ptr<CaptureJob>& job) {
        std::scoped_lock lock{mutex};
        std::erase(pending, job);
    }

    std::deque<std::shared_ptr<CaptureJob>> take() {
        std::scoped_lock lock{mutex};
        return std::exchange(pending, {});
    }

private:
    std::mutex                              mutex;
    HWND                                    window = nullptr;
    bool                                    opened = false;
    std::deque<std::shared_ptr<CaptureJob>> pending;
};

inline void answer(CaptureJob& job, CaptureResult result) noexcept {
    try {
        job.done.set_value(std::move(result));
    } catch (...) { }
}

// The PNG that CapturePreview wrote to its memory stream.
inline CaptureResult pngBytes(HRESULT result, IStream* stream) {
    if (FAILED(result))
        return std::unexpected(std::format("capture failed (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result)));
    STATSTG statistics{};
    check(stream->Stat(&statistics, STATFLAG_NONAME), "measuring the capture");
    constexpr ULONGLONG limit = 256ull * 1024 * 1024;
    if (!statistics.cbSize.QuadPart || statistics.cbSize.QuadPart > limit)
        return std::unexpected("the engine returned no usable image");
    check(stream->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr), "rewinding the capture");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(statistics.cbSize.QuadPart));
    ULONG                     read = 0;
    check(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read), "reading the capture");
    bytes.resize(read);
    return bytes;
}

inline CaptureProvider captureProvider(std::shared_ptr<CaptureQueue> queue) {
    return [queue = std::move(queue)](std::chrono::milliseconds timeout) -> CaptureResult {
        auto job    = std::make_shared<CaptureJob>();
        auto future = job->done.get_future();
        if (!queue->submit(job))
            return std::unexpected("the window is closed or not ready");
        if (future.wait_for(timeout) != std::future_status::ready) {
            queue->cancel(job);
            return std::unexpected("the window did not answer within the capture timeout");
        }
        return future.get();
    };
}

// All members and callbacks are used on the caller's STA. Pending creation callbacks hold only weak
// references, so closing during initialization cannot access a destroyed window or stack frame.
class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
        if (timeout.count() <= 0 || timeout.count() > USER_TIMER_MAXIMUM)
            throw std::invalid_argument("WebView2: startup timeout must be between 1 and USER_TIMER_MAXIMUM milliseconds");
        startupTimeoutMilliseconds = static_cast<UINT>(timeout.count());
    }
    HWND                                             window = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>            view;
    std::exception_ptr                               failure;
    std::wstring                                     url;
    webfront::detail::BoundContentPolicy             contentPolicy;
    // Internal adapter observer, used by lifecycle probes after a successful native cancellation.
    std::function<void()>                            navigationCancelled;
    // Internal adapter observer: the browser exists and nothing is loaded yet (the transport benchmark of ADR-017 registers
    // its message channel here).
    std::function<void()>                            viewReady;
    bool                                             closed       = false;
    UINT_PTR                                         startupTimer = 0;
    std::shared_ptr<CaptureQueue>                    captureQueue = std::make_shared<CaptureQueue>();
    CaptureRegistration                              captureRegistration;

    ~Session() {
        close();
        if (window)
            DestroyWindow(window);
        if (registered)
            UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));
    }

    void navigationStarting(ICoreWebView2NavigationStartingEventArgs* args) noexcept {
        if (closed)
            return;
        try {
            LPWSTR raw = nullptr;
            check(args->get_Uri(&raw), "reading navigation URL");
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> uri(raw, CoTaskMemFree);
            if (contentPolicy.allowsNavigation(utf8(uri.get())))
                return;
            check(args->put_Cancel(TRUE), "blocking navigation");
            if (navigationCancelled)
                navigationCancelled();
        } catch (...) {
            fail();
        }
    }

    void newWindowRequested(ICoreWebView2NewWindowRequestedEventArgs* args, browser::Launcher launch = browser::launch) noexcept {
        try {
            check(args->put_Handled(TRUE), "blocking unmanaged popup");
            BOOL userInitiated = FALSE;
            check(args->get_IsUserInitiated(&userInitiated), "reading popup user initiation");
            LPWSTR raw = nullptr;
            check(args->get_Uri(&raw), "reading popup URL");
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> uri(raw, CoTaskMemFree);
            const auto                                               popupUrl = utf8(uri.get());
            // A failed external launcher must not end the application session.
            try {
                contentPolicy.handlePopup(popupUrl, userInitiated != FALSE, launch);
            } catch (const std::exception& error) {
                log::error("WebView2 popup launch failed: {}", error.what());
            } catch (...) {
                log::error("WebView2 popup launch failed with a non-standard exception");
            }
        } catch (...) {
            fail();
        }
    }

    void close() noexcept {
        stopStartupTimer();
        if (closed)
            return;
        closed              = true;
        captureRegistration = {};
        for (const auto& job : captureQueue->close())
            answer(*job, std::unexpected("the window is closed"));
        try {
            if (controller)
                check(controller->Close(), "closing controller");
        } catch (...) {
            if (!failure)
                failure = std::current_exception();
        }
        view.Reset();
        controller.Reset();
        environment.Reset();
    }

    void fail() noexcept {
        stopStartupTimer();
        if (!failure)
            failure = std::current_exception();
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
    }

    void run() {
        // A broken runtime may never complete creation. Closing the window must still return;
        // late callbacks hold weak references and cannot revive a destroyed session.
        while (window) {
            if (!dispatchNextMessage())
                break;
        }
        if (failure)
            std::rethrow_exception(failure);
    }

    bool loadingInitialPage() const noexcept {
        return initialNavigationPending;
    }

    static void checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND kind) {
        // An unresponsive renderer can recover; WebView2 also recovers auxiliary processes itself.
        // Main renderer/browser exits deliberately fail the session: automatic reload could repeat application actions.
        if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED)
            throw std::runtime_error("WebView2: browser process failed (kind " + std::to_string(static_cast<int>(kind)) + ")");
    }

    void resize() {
        if (!hasLiveController())
            return;
        RECT bounds{};
        if (!GetClientRect(window, &bounds))
            throw std::runtime_error("WebView2: cannot read window bounds");
        check(controller->put_Bounds(bounds), "resizing controller");
    }

    void createWindow(bool visible = true, WindowOptions settings = {}) {
        windowOptions = frontend::detail::resolveWindow(std::move(settings));
        className = L"WebFront.WebView2." + std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc   = windowProcedure;
        windowClass.hInstance     = GetModuleHandleW(nullptr);
        windowClass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        windowClass.lpszClassName = className.c_str();
        windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        if (!RegisterClassW(&windowClass))
            throw std::runtime_error("WebView2: cannot register window class");
        registered = true;
        window     = CreateWindowExW(0,
                                     className.c_str(),
                                     utf16(windowOptions.title).c_str(),
                                     WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     windowOptions.width,
                                     windowOptions.height,
                                     nullptr,
                                     nullptr,
                                     windowClass.hInstance,
                                     this);
        if (!window)
            throw std::runtime_error("WebView2: cannot create window");
        // Bounds initialization errors and an unresponsive runtime must not leave an empty window forever.
        startupTimer = SetTimer(nullptr, 0, startupTimeoutMilliseconds, nullptr);
        if (!startupTimer)
            throw std::runtime_error("WebView2: cannot start startup timer");
        if (visible)
            ShowWindow(window, SW_SHOW);
    }

    void start(const std::filesystem::path& profile, EnvironmentFactory createEnvironment = CreateCoreWebView2EnvironmentWithOptions) {
        auto       weak    = weak_from_this();
        const auto started = createEnvironment(
            nullptr,
            profile.c_str(),
            nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([weak](HRESULT                   result,
                                                                                                        ICoreWebView2Environment* created) -> HRESULT {
                const auto self = weak.lock();
                if (!self)
                    return S_OK;
                if (self->closed)
                    return S_OK;
                self->environmentCreated(result, created);
                return S_OK;
            }).Get());
        check(started, "starting environment creation");
    }

private:
    std::wstring className;
    WindowOptions windowOptions;
    bool         registered                 = false;
    bool         initialNavigationPending   = true;
    UINT         startupTimeoutMilliseconds = 30'000;

    void requestWindowClose() noexcept {
        // Navigation cancelled by the page's own close request is a normal shutdown.
        initialNavigationPending = false;
        stopStartupTimer();
        if (window)
            PostMessageW(window, WM_CLOSE, 0, 0);
    }

    bool hasLiveController() const noexcept {
        return controller && !closed;
    }

    static bool receivedSuccessfulHttpResponse(ICoreWebView2NavigationCompletedEventArgs* args) {
        Microsoft::WRL::ComPtr<ICoreWebView2NavigationCompletedEventArgs2> response;
        if (FAILED(args->QueryInterface(IID_PPV_ARGS(response.GetAddressOf()))))
            return false;
        int status = 0;
        check(response->get_HttpStatusCode(&status), "reading navigation HTTP status");
        return status >= 200 && status < 400;
    }

    void stopStartupTimer() noexcept {
        if (startupTimer)
            KillTimer(nullptr, startupTimer);
        startupTimer = 0;
    }

    void checkStartupTimeout(const MSG& message) const {
        // close() and fail() clear the timer, including messages queued before KillTimer().
        if (!startupTimer)
            return;
        if (message.hwnd)
            return;
        if (message.message != WM_TIMER)
            return;
        if (message.wParam == startupTimer)
            throw std::runtime_error(std::format("WebView2: runtime startup timed out after {} milliseconds", startupTimeoutMilliseconds));
    }

    bool dispatchNextMessage() const {
        MSG        message{};
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
            throw std::runtime_error("WebView2: message loop failed");
        if (!result) {
            PostQuitMessage(static_cast<int>(message.wParam));
            return false;
        }
        checkStartupTimeout(message);
        TranslateMessage(&message);
        DispatchMessageW(&message);
        return true;
    }

    void environmentCreated(HRESULT result, ICoreWebView2Environment* created) noexcept {
        try {
            check(result, "creating environment");
            if (!created)
                throw std::runtime_error("WebView2: no environment returned");
            environment = created;
            createController();
        } catch (...) {
            fail();
        }
    }

    void controllerCreated(HRESULT result, ICoreWebView2Controller* created) noexcept {
        if (closed) {
            if (created)
                created->Close();
            return;
        }
        try {
            check(result, "creating controller");
            if (!created)
                throw std::runtime_error("WebView2: no controller returned");
            controller = created;
            check(created->get_CoreWebView2(&view), "getting browser");
            configure();
        } catch (...) {
            fail();
        }
    }

    void processFailed(ICoreWebView2ProcessFailedEventArgs* args) noexcept {
        if (closed)
            return;
        try {
            COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
            check(args->get_ProcessFailedKind(&kind), "reading browser process failure");
            checkProcessFailure(kind);
        } catch (...) {
            fail();
        }
    }

    void handleInitialNavigationFailure(ICoreWebView2NavigationCompletedEventArgs* args) {
        COREWEBVIEW2_WEB_ERROR_STATUS status{};
        check(args->get_WebErrorStatus(&status), "reading navigation error");
        if (status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
            return;
        // window.stop()/window.close() can abort loading after a successful HTTP response.
        if (status == COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED && receivedSuccessfulHttpResponse(args)) {
            initialNavigationPending = false;
            return;
        }
        throw std::runtime_error("WebView2: navigation failed (WebErrorStatus " + std::to_string(static_cast<int>(status))
                                 + "); check the local server and page path");
    }

    void navigationCompleted(ICoreWebView2NavigationCompletedEventArgs* args) noexcept {
        if (closed || !initialNavigationPending)
            return;
        try {
            BOOL success = FALSE;
            check(args->get_IsSuccess(&success), "reading navigation result");
            if (!success) {
                handleInitialNavigationFailure(args);
                return;
            }
            initialNavigationPending = false;
        } catch (...) {
            fail();
        }
    }

    void restoreFocus() {
        if (hasLiveController())
            check(controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC), "restoring browser focus");
    }

    void startCaptures() noexcept {
        for (const auto& job : captureQueue->take()) {
            try {
                if (!hasLiveController()) {
                    answer(*job, std::unexpected("the window is closed"));
                    continue;
                }
                Microsoft::WRL::ComPtr<IStream> stream;
                check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "creating the capture stream");
                // The handler keeps the job and the stream until the engine answers.
                check(view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                                           stream.Get(),
                                           Microsoft::WRL::Callback<ICoreWebView2CapturePreviewCompletedHandler>([job, stream](HRESULT result) -> HRESULT {
                                               try {
                                                   answer(*job, pngBytes(result, stream.Get()));
                                               } catch (...) {
                                                   try {
                                                       job->done.set_exception(std::current_exception());
                                                   } catch (...) { }
                                               }
                                               return S_OK;
                                           }).Get()),
                      "starting the capture");
            } catch (...) {
                try {
                    job->done.set_exception(std::current_exception());
                } catch (...) { }
            }
        }
    }

    void notifyPositionChanged() {
        if (hasLiveController())
            check(controller->NotifyParentWindowPositionChanged(), "notifying parent window position");
    }

    void applyDpiBounds(HWND handle, LPARAM lParam) {
        const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
        if (!SetWindowPos(handle, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE))
            throw std::runtime_error("WebView2: cannot apply DPI window bounds");
    }

    bool handleWindowMessage(HWND handle, UINT message, LPARAM lParam) {
        switch (message) {
            case WM_DPICHANGED:
                applyDpiBounds(handle, lParam);
                return true;
            case WM_SIZE:
                resize();
                return true;
            case WM_SETFOCUS:
                restoreFocus();
                return true;
            case WM_MOVE:
                notifyPositionChanged();
                return true;
            case captureMessage:
                startCaptures();
                return true;
            case WM_CLOSE:
                close();
                DestroyWindow(handle);
                return true;
            case WM_NCDESTROY:
                close();
                window = nullptr;
                SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
                return false;
            default:
                return false;
        }
    }

    static LRESULT CALLBACK windowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        auto* self = reinterpret_cast<Session*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Session*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window = handle;
        }
        if (self) {
            try {
                if (self->handleWindowMessage(handle, message, lParam))
                    return 0;
            } catch (...) {
                self->fail();
                return 0;
            }
        }
        return DefWindowProcW(handle, message, wParam, lParam);
    }

    void createController() {
        auto       weak    = weak_from_this();
        const auto started = environment->CreateCoreWebView2Controller(
            window,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([weak](HRESULT                  result,
                                                                                                       ICoreWebView2Controller* created) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->controllerCreated(result, created);
                } else if (created) {
                    created->Close();
                }
                return S_OK;
            }).Get());
        check(started, "starting controller creation");
    }

    void registerLifecycleHandlers() {
        auto                   weak = weak_from_this();
        EventRegistrationToken token{};
        check(
            view->add_ProcessFailed(
                Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>([weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                    if (const auto self = weak.lock())
                        self->processFailed(args);
                    return S_OK;
                }).Get(),
                &token),
            "registering browser process failure handler");
        check(view->add_WindowCloseRequested(
                  Microsoft::WRL::Callback<ICoreWebView2WindowCloseRequestedEventHandler>([weak](ICoreWebView2*, IUnknown*) -> HRESULT {
                      if (const auto self = weak.lock())
                          self->requestWindowClose();
                      return S_OK;
                  }).Get(),
                  &token),
              "registering window close handler");
        check(view->add_NavigationCompleted(
                  Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>([weak](ICoreWebView2*,
                                                                                                ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                      if (const auto self = weak.lock())
                          self->navigationCompleted(args);
                      return S_OK;
                  }).Get(),
                  &token),
              "registering navigation handler");
    }

    void registerPolicyHandlers() {
        auto                                          weak = weak_from_this();
        EventRegistrationToken                        token{};
        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        check(view->get_Settings(&settings), "getting browser settings");
        configureContentSettings(settings.Get(), contentPolicy.options());
        check(view->add_NavigationStarting(
                  Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>([weak](ICoreWebView2*,
                                                                                               ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                      if (const auto self = weak.lock())
                          self->navigationStarting(args);
                      return S_OK;
                  }).Get(),
                  &token),
              "registering navigation policy");
        check(view->add_NewWindowRequested(
                  Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>([weak](ICoreWebView2*,
                                                                                               ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                      if (const auto self = weak.lock())
                          self->newWindowRequested(args);
                      return S_OK;
                  }).Get(),
                  &token),
              "registering popup policy");
    }

    void configure() {
        registerLifecycleHandlers();
        registerPolicyHandlers();
        resize();
        check(controller->put_IsVisible(TRUE), "showing controller");
        if (GetFocus() == window)
            restoreFocus();
        // Runtime creation is complete. Slow page resources must not consume its watchdog budget.
        stopStartupTimer();
        captureQueue->open(window);
        captureRegistration = CaptureRegistration{captureProvider(captureQueue)};
        if (viewReady)
            viewReady();
        check(view->Navigate(url.c_str()), "navigating to application");
    }
};

inline void open(std::string_view address,
                 std::string_view port,
                 std::string_view file,
                 const WebView2Options& options,
                 const std::function<void(Session&)>& prepare = {}) {
    auto contentPolicy = webfront::detail::BoundContentPolicy{frontend::detail::applicationUrl(address, port, ""), options.contentPolicy};
    Apartment                                                apartment;
    LPWSTR                                                   rawVersion = nullptr;
    const auto                                               available  = GetAvailableCoreWebView2BrowserVersionString(nullptr, &rawVersion);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> version(rawVersion, CoTaskMemFree);
    if (FAILED(available) || !version)
        throw std::runtime_error("WebView2 runtime not found. Install Microsoft Edge WebView2 Evergreen Runtime: "
                                 "https://developer.microsoft.com/microsoft-edge/webview2/");
    auto session       = std::make_shared<Session>(options.startupTimeout);
    session->contentPolicy = std::move(contentPolicy);
    session->url       = applicationUrl(address, port, file);
    if (prepare)
        prepare(*session);
    const auto profile = options.userDataFolder.empty() ? userDataFolder() : std::filesystem::absolute(options.userDataFolder);
    std::filesystem::create_directories(profile);
    session->createWindow(options.visible, options.window);
    session->start(profile);
    session->run();
}

}  // namespace webfront::frontend::detail::webview2
#endif

namespace webfront::frontend {

/** @brief Optional Windows embedded frontend. open() owns an STA and blocks until the window closes. */
struct WebView2Frontend {
    static constexpr Action action{Action::closeServerAfterOpen};

    static void initialize() { }

    static void open(std::string_view port, std::string_view file) {
        open("127.0.0.1", port, file);
    }

    static void open(std::string_view address, std::string_view port, std::string_view file, const WebView2Options& options = {}) {
#if defined(WEBFRONT_EMBED_WEBVIEW2) && defined(_WIN32)
        detail::webview2::open(address, port, file, options);
#else
        (void)address;
        (void)port;
        (void)file;
        (void)options;
        throw std::runtime_error("WebView2Frontend is unavailable. Configure on Windows with -DWEBFRONT_EMBED_WEBVIEW2=ON");
#endif
    }
    template <std::same_as<ContentPolicy> Policy>
    static void open(std::string_view address, std::string_view port, std::string_view file, const Policy& policy) {
        WebView2Options options;
        options.contentPolicy = policy;
        open(address, port, file, options);
    }
    static void open(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy& policy, const WindowOptions& window) {
        WebView2Options options;
        options.contentPolicy = policy;
        options.window        = window;
        open(address, port, file, options);
    }
};

/** @brief Configured WebView2 frontend. OptionsProvider::get() returns per-opening WebView2Options. */
template <typename OptionsProvider>
struct WebView2FrontendWithOptions : WebView2Frontend {
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page);
    }
    static void open(std::string_view address, std::string_view port, std::string_view page) {
        WebView2Frontend::open(address, port, page, OptionsProvider::get());
    }
    template <std::same_as<ContentPolicy> Policy>
    static void open(std::string_view address, std::string_view port, std::string_view page, const Policy& policy) {
        auto options          = OptionsProvider::get();
        options.contentPolicy = policy;
        WebView2Frontend::open(address, port, page, options);
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy, const WindowOptions& window) {
        auto options          = OptionsProvider::get();
        options.contentPolicy = policy;
        options.window        = window;
        WebView2Frontend::open(address, port, page, options);
    }
};

}  // namespace webfront::frontend
