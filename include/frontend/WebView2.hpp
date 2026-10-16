#pragma once

#include "Action.hpp"

#include <stdexcept>
#include <string_view>

#if defined(WEBFRONT_EMBED_WEBVIEW2) && defined(_WIN32)
    #include "../system/WindowsCompat.hpp"

    #include <cstdint>
    #include <exception>
    #include <filesystem>
    #include <format>
    #include <limits>
    #include <memory>
    #include <string>

    #include <shlobj.h>
    #include <WebView2.h>
    #include <wrl.h>

namespace webfront::frontend::detail::webview2 {

inline constexpr UINT startupTimeoutMilliseconds = 30'000;
using EnvironmentFactory                         = decltype(&CreateCoreWebView2EnvironmentWithOptions);

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

inline std::wstring applicationUrl(std::string_view port, std::string_view file) {
    std::string    url   = "http://localhost:" + std::string(port) + "/";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char byte : file) {
        constexpr std::string_view pathCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
        if (pathCharacters.find(static_cast<char>(byte)) != std::string_view::npos) {
            url += static_cast<char>(byte);
        } else {
            url += '%';
            url += hex[byte >> 4];
            url += hex[byte & 15];
        }
    }
    return utf16(url);
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

// All members and callbacks are used on the caller's STA. Pending creation callbacks hold only weak
// references, so closing during initialization cannot access a destroyed window or stack frame.
class Session : public std::enable_shared_from_this<Session> {
public:
    HWND                                             window = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>            view;
    std::exception_ptr                               failure;
    std::wstring                                     url;
    bool                                             closed       = false;
    UINT_PTR                                         startupTimer = 0;

    ~Session() {
        close();
        if (window)
            DestroyWindow(window);
        if (registered)
            UnregisterClassW(className.c_str(), GetModuleHandleW(nullptr));
    }

    void close() noexcept {
        stopStartupTimer();
        closed = true;
        if (controller)
            controller->Close();
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

    static void checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND kind) {
        // An unresponsive renderer can recover; WebView2 also recovers auxiliary processes itself.
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

    void createWindow() {
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
                                     L"WebFront Application",
                                     WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     1200,
                                     800,
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
    bool         registered = false;

    bool hasLiveController() const noexcept {
        return controller && !closed;
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
            throw std::runtime_error(std::format("WebView2: startup timed out after {} seconds", startupTimeoutMilliseconds / 1000));
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

    void navigationCompleted(ICoreWebView2NavigationCompletedEventArgs* args) noexcept {
        if (closed || !startupTimer)
            return;
        try {
            BOOL success = FALSE;
            check(args->get_IsSuccess(&success), "reading navigation result");
            if (!success) {
                COREWEBVIEW2_WEB_ERROR_STATUS status{};
                check(args->get_WebErrorStatus(&status), "reading navigation error");
                throw std::runtime_error("WebView2: navigation failed (WebErrorStatus " + std::to_string(static_cast<int>(status))
                                         + "); check the local server and page path");
            }
            stopStartupTimer();
        } catch (...) {
            fail();
        }
    }

    void restoreFocus() {
        if (hasLiveController())
            check(controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC), "restoring browser focus");
    }

    void notifyPositionChanged() {
        if (hasLiveController())
            check(controller->NotifyParentWindowPositionChanged(), "notifying parent window position");
    }

    bool handleWindowMessage(HWND handle, UINT message) {
        switch (message) {
            case WM_SIZE:
                resize();
                return true;
            case WM_SETFOCUS:
                restoreFocus();
                return true;
            case WM_MOVE:
                notifyPositionChanged();
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
                if (self->handleWindowMessage(handle, message))
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

    void configure() {
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
                      if (const auto self = weak.lock(); self && self->window)
                          PostMessageW(self->window, WM_CLOSE, 0, 0);
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
        resize();
        check(controller->put_IsVisible(TRUE), "showing controller");
        if (GetFocus() == window)
            restoreFocus();
        check(view->Navigate(url.c_str()), "navigating to application");
    }
};

inline void open(std::string_view port, std::string_view file) {
    Apartment                                                apartment;
    LPWSTR                                                   rawVersion = nullptr;
    const auto                                               available  = GetAvailableCoreWebView2BrowserVersionString(nullptr, &rawVersion);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> version(rawVersion, CoTaskMemFree);
    if (FAILED(available) || !version)
        throw std::runtime_error("WebView2 runtime not found. Install Microsoft Edge WebView2 Evergreen Runtime: "
                                 "https://developer.microsoft.com/microsoft-edge/webview2/");
    auto session       = std::make_shared<Session>();
    session->url       = applicationUrl(port, file);
    const auto profile = userDataFolder();
    session->createWindow();
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
#if defined(WEBFRONT_EMBED_WEBVIEW2) && defined(_WIN32)
        detail::webview2::open(port, file);
#else
        (void)port;
        (void)file;
        throw std::runtime_error("WebView2Frontend is unavailable. Configure on Windows with -DWEBFRONT_EMBED_WEBVIEW2=ON");
#endif
    }
};

}  // namespace webfront::frontend
