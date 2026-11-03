#include "../driver/TestFrontend.hpp"
#include <webfront/system/NativeFS.hpp>
#include <webfront/utils/Json.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <semaphore>
#include <string>
#include <thread>

bool webView2ClosesServer();

namespace {

using TestFS = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
struct LifecycleOptions {
    static webfront::frontend::WebView2Options get() {
        return {.startupTimeout = std::chrono::seconds{90}, .visible = webfront::testdriver::diagnosticWindows()};
    }
};
using App = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, webfront::frontend::WebView2FrontendWithOptions<LifecycleOptions>>;

// Dispatch test actions on the same STA as the production message loop.
class Actions {
public:
    std::function<void(UINT)> dispatch;
    std::function<void()>     onFailure;
    std::exception_ptr        failure;
    HWND                      window = nullptr;

    Actions() {
        WNDCLASSW type{};
        type.hInstance     = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WebFront.LifecycleActions";
        type.lpfnWndProc   = windowProcedure;
        if (!RegisterClassW(&type))
            throw std::runtime_error("Cannot register lifecycle actions");
        window = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
        if (!window)
            throw std::runtime_error("Cannot create lifecycle actions");
    }
    ~Actions() {
        DestroyWindow(window);
        UnregisterClassW(L"WebFront.LifecycleActions", GetModuleHandleW(nullptr));
    }
    Actions(const Actions&)            = delete;
    Actions& operator=(const Actions&) = delete;

private:
    static LRESULT CALLBACK windowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto* self = reinterpret_cast<Actions*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(handle, message, wParam, lParam);
        if (message == WM_APP || message == WM_TIMER) {
            try {
                self->dispatch(message);
            } catch (...) {
                self->failure = std::current_exception();
                if (self->onFailure)
                    self->onFailure();
            }
            return 0;
        }
        return DefWindowProcW(handle, message, wParam, lParam);
    }
};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void failWithMessage(webfront::frontend::detail::webview2::Session& session, const char* message) noexcept {
    try {
        throw std::runtime_error(message);
    } catch (...) {
        session.fail();
    }
}

bool failedNavigationKeptWindowOpen(const webfront::frontend::detail::webview2::Session& session, ICoreWebView2NavigationCompletedEventArgs* args) noexcept {
    BOOL success = TRUE;
    if (FAILED(args->get_IsSuccess(&success)))
        return false;
    if (success)
        return false;
    if (!IsWindow(session.window))
        return false;
    return !session.failure;
}

struct DeferredEnvironment {
    static inline Microsoft::WRL::ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> completion;

    static HRESULT STDAPICALLTYPE create(PCWSTR,
                                         PCWSTR,
                                         ICoreWebView2EnvironmentOptions*,
                                         ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* callback) {
        completion = callback;
        return S_OK;  // The asynchronous creation request is accepted but deliberately never completed.
    }
};

int startupCompletion(bool failed) {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto      session = std::make_shared<Session>();
    session->createWindow(webfront::testdriver::diagnosticWindows());
    require(static_cast<bool>(IsWindowVisible(session->window)) == webfront::testdriver::diagnosticWindows(), "Window visibility option was ignored");
    session->start(L"unused-profile", DeferredEnvironment::create);
    require(DeferredEnvironment::completion.Get() != nullptr, "Creation request was not started");
    const auto timer = session->startupTimer;
    Actions    actions;
    bool       fallback = false;
    actions.dispatch    = [&](UINT) {
        fallback = true;
        PostQuitMessage(0);
    };
    // Make the old unbounded loop fail quickly instead of hanging this regression test.
    require(SetTimer(actions.window, 1, 100, nullptr) != 0, "Cannot start regression watchdog");
    if (failed) {
        failWithMessage(*session, "original startup failure");
        failWithMessage(*session, "secondary startup failure");
    } else {
        session->close();
    }
    // A killed timer can still have a queued message. Creation deliberately never completes.
    PostThreadMessageW(GetCurrentThreadId(), WM_TIMER, timer, 0);
    PostMessageW(session->window, WM_CLOSE, 0, 0);
    bool threw = false;
    try {
        session->run();
    } catch (const std::exception& error) {
        threw = true;
        require(failed, "Normal closure unexpectedly threw");
        require(std::string_view(error.what()) == "original startup failure", "Startup error was replaced");
    }
    require(threw == failed, "Unexpected startup result");
    require(!fallback, "Closure waited for a creation callback that never arrived");
    require(!session->startupTimer, "Startup timer was not cancelled");
    const std::weak_ptr<Session> weak = session;
    session.reset();
    require(weak.expired(), "A pending creation callback kept the session alive");
    auto completion = std::move(DeferredEnvironment::completion);
    check(completion->Invoke(E_ABORT, nullptr), "late environment callback after session destruction");
    return 0;
}

int lateNavigationFailure() {
    using namespace webfront::frontend::detail::webview2;
    // Start on allowed HTTP content; about:blank is deliberately outside the content policy.
    webfront::http::Server<webfront::NetProvider, webfront::fs::NativeDebugFS> server("127.0.0.1", "9002", std::filesystem::current_path());
    std::jthread                                                               worker([&](std::stop_token stop) {
        std::stop_callback cleanup(stop, [&] {
            server.stop();
        });
        server.run();
    });
    Apartment                                                                  apartment;
    auto                                                                       session = std::make_shared<Session>();
    session->url                                                                       = L"http://127.0.0.1:9002/policy-controls.html";
    // Reach the engine's network-failure path rather than the navigation-policy rejection path.
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {.navigationAllowedOrigins = {"http://127.0.0.1:0"}}};
    session->createWindow(webfront::testdriver::diagnosticWindows());
    require(static_cast<bool>(IsWindowVisible(session->window)) == webfront::testdriver::diagnosticWindows(), "Window visibility option was ignored");
    Actions actions;
    actions.onFailure = [&] {
        session->fail();
    };
    bool observed    = false;
    actions.dispatch = [&](UINT) {
        if (!session->view || session->loadingInitialPage())
            return;
        KillTimer(actions.window, 1);
        EventRegistrationToken token{};
        check(session->view->add_NavigationCompleted(
                  Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>([&](ICoreWebView2*,
                                                                                             ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                      // Session's production NavigationCompleted handler was registered first.
                      // This observer only checks the real failed-navigation outcome.
                      observed = failedNavigationKeptWindowOpen(*session, args);
                      PostMessageW(session->window, WM_CLOSE, 0, 0);
                      return S_OK;
                  }).Get(),
                  &token),
              "registering regression observer");
        check(session->view->Navigate(L"http://127.0.0.1:0/offline"), "starting later navigation");
    };
    if (!SetTimer(actions.window, 1, 10, nullptr))
        throw std::runtime_error("Cannot start lifecycle actions");
    session->start(userDataFolder());
    session->run();
    if (actions.failure)
        std::rethrow_exception(actions.failure);
    return observed ? 0 : 1;
}

// Exercise the engine's chooser and download paths. DevTools supplies the chooser answer because
// WebView2 exposes no file-dialog callback; DownloadStarting supplies a private destination.
int fileServices(std::string_view mode) {
    using namespace webfront::frontend::detail::webview2;
    const auto folder = std::filesystem::temp_directory_path()
                        / ("webfront-webview2-files-" + std::to_string(GetCurrentProcessId()) + "-"
                           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(folder), "Cannot create private test directory");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};
    const auto fixture = folder / "fixture.txt";
    std::ofstream(fixture) << "chosen by the test";
    const auto destination = folder / "result.txt";

    webfront::http::Server<webfront::NetProvider, webfront::fs::NativeDebugFS> server("127.0.0.1", "9002", std::filesystem::current_path());
    std::jthread                                                               worker([&](std::stop_token stop) {
        std::stop_callback cleanupServer(stop, [&] {
            server.stop();
        });
        server.run();
    });
    Apartment                                                                  apartment;
    auto                                                                       session = std::make_shared<Session>();
    session->url           = mode == "download" ? L"http://127.0.0.1:9002/file-services.html?download" : L"http://127.0.0.1:9002/file-services.html?choose";
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {}};
    session->createWindow(webfront::testdriver::diagnosticWindows());
    Actions actions;
    actions.onFailure = [&] {
        session->fail();
    };
    bool                                                   ready = false, interceptRequested = false, clicked = false, asked = false, finished = false;
    Microsoft::WRL::ComPtr<ICoreWebView2DownloadOperation> download;
    Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> chooser;
    session->viewReady = [&] {
        EventRegistrationToken token{};
        if (mode == "download") {
            Microsoft::WRL::ComPtr<ICoreWebView2_4> view4;
            check(session->view.As(&view4), "querying download support");
            check(view4->add_DownloadStarting(
                      Microsoft::WRL::Callback<ICoreWebView2DownloadStartingEventHandler>([&](ICoreWebView2*,
                                                                                              ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                          try {
                              check(args->get_DownloadOperation(&download), "getting download operation");
                              check(args->put_ResultFilePath(destination.c_str()), "setting download destination");
                              check(args->put_Handled(TRUE), "suppressing download UI");
                          } catch (...) {
                              session->fail();
                          }
                          return S_OK;
                      }).Get(),
                      &token),
                  "registering download handler");
            ready = true;
            return;
        }
        check(session->view->GetDevToolsProtocolEventReceiver(L"Page.fileChooserOpened", &chooser), "getting chooser event");
        check(chooser->add_DevToolsProtocolEventReceived(
                  Microsoft::WRL::Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
                      [&](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
                          try {
                              LPWSTR raw = nullptr;
                              check(args->get_ParameterObjectAsJson(&raw), "reading chooser event");
                              const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> json(raw, CoTaskMemFree);
                              std::wcmatch                                             match;
                              require(std::regex_search(json.get(), match, std::wregex(L"\\\"backendNodeId\\\"\\s*:\\s*(\\d+)")),
                                      "Chooser event has no input node");
                              asked = true;
                              if (mode == "cancel")
                                  return S_OK;  // The interception was configured to cancel and emit the page's cancel event.
                              const auto files      = webfront::json::quote(utf8(fixture.c_str()));
                              const auto parameters = utf16("{\"backendNodeId\":" + utf8(match[1].str()) + ",\"files\":[" + files + "]}");
                              check(session->view->CallDevToolsProtocolMethod(L"DOM.setFileInputFiles", parameters.c_str(), nullptr), "answering chooser");
                          } catch (...) {
                              session->fail();
                          }
                          return S_OK;
                      })
                      .Get(),
                  &token),
              "registering chooser handler");
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    actions.dispatch    = [&](UINT) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("File service scenario timed out: ready=" + std::to_string(ready) + " clicked=" + std::to_string(clicked)
                                     + " asked=" + std::to_string(asked) + " loading=" + std::to_string(session->loadingInitialPage()));
        if (!session->view || session->loadingInitialPage())
            return;
        if (mode != "download" && !interceptRequested) {
            interceptRequested = true;
            check(session->view->CallDevToolsProtocolMethod(L"Page.enable", L"{}", nullptr), "enabling page events");
            const auto parameters = mode == "cancel" ? L"{\"enabled\":true,\"cancel\":true}" : L"{\"enabled\":true}";
            check(session->view->CallDevToolsProtocolMethod(
                      L"Page.setInterceptFileChooserDialog",
                      parameters,
                      Microsoft::WRL::Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([&](HRESULT result, LPCWSTR) -> HRESULT {
                          try {
                              check(result, "enabling chooser interception");
                              ready = true;
                          } catch (...) {
                              session->fail();
                          }
                          return S_OK;
                      }).Get()),
                  "enabling chooser interception");
        }
        if (!ready)
            return;
        if (mode == "download") {
            if (!download)
                return;
            COREWEBVIEW2_DOWNLOAD_STATE state{};
            check(download->get_State(&state), "reading download state");
            require(state != COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED, "Download was interrupted");
            finished = state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED;
        } else {
            if (!clicked) {
                clicked = true;
                check(session->view->CallDevToolsProtocolMethod(L"Runtime.evaluate",
                                                                L"{\"expression\":\"document.getElementById('picker').click()\",\"userGesture\":true}",
                                                                nullptr),
                      "opening chooser");
            }
            LPWSTR raw = nullptr;
            check(session->view->get_DocumentTitle(&raw), "reading page result");
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> title(raw, CoTaskMemFree);
            finished = asked && std::wstring_view(title.get()) == (mode == "choose" ? L"picked:chosen by the test" : L"cancelled");
        }
        if (finished)
            PostMessageW(session->window, WM_CLOSE, 0, 0);
    };
    require(SetTimer(actions.window, 1, 20, nullptr) != 0, "Cannot start file service watchdog");
    session->start(userDataFolder());
    session->run();
    if (actions.failure)
        std::rethrow_exception(actions.failure);
    require(finished, "File service did not finish");
    if (mode == "download") {
        std::ifstream stream(destination, std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(stream), {}) == "downloaded by WebFront", "Wrong downloaded content");
    }
    return 0;
}

// alert, confirm and prompt reach the injection point and their answers reach the page (issue #452).
int scriptDialogs() {
    using namespace webfront::frontend::detail::webview2;
    webfront::http::Server<webfront::NetProvider, webfront::fs::NativeDebugFS> server("127.0.0.1", "9002", std::filesystem::current_path());
    std::jthread                                                               worker([&](std::stop_token stop) {
        std::stop_callback cleanupServer(stop, [&] {
            server.stop();
        });
        server.run();
    });
    Apartment                                                                  apartment;
    auto                                                                       session = std::make_shared<Session>();
    session->url           = L"http://127.0.0.1:9002/script-dialogs.html";
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {}};
    std::vector<std::string> seen;
    session->answerScriptDialog = [&](const webfront::frontend::ScriptDialog& dialog) {
        using Kind = webfront::frontend::ScriptDialog::Kind;
        seen.push_back(std::to_string(static_cast<int>(dialog.kind)) + ":" + dialog.message + ":" + dialog.defaultText);
        if (dialog.kind == Kind::prompt)
            return dialog.message == "name?" ? webfront::frontend::ScriptDialogAnswer{true, "typed"} : webfront::frontend::ScriptDialogAnswer{};
        return webfront::frontend::ScriptDialogAnswer{true, {}};
    };
    session->createWindow(webfront::testdriver::diagnosticWindows());
    Actions actions;
    actions.onFailure = [&] {
        session->fail();
    };
    std::wstring title;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    actions.dispatch    = [&](UINT) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Script dialog scenario timed out");
        if (!session->view)
            return;
        LPWSTR raw = nullptr;
        check(session->view->get_DocumentTitle(&raw), "reading page result");
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> current(raw, CoTaskMemFree);
        title = current.get();
        if (title.starts_with(L"answers:"))
            PostMessageW(session->window, WM_CLOSE, 0, 0);
    };
    require(SetTimer(actions.window, 1, 20, nullptr) != 0, "Cannot start script dialog watchdog");
    session->start(userDataFolder());
    session->run();
    if (actions.failure)
        std::rethrow_exception(actions.failure);
    require(title == L"answers:true|typed|null", "Unexpected answers from the script dialogs");
    const std::vector<std::string> expected{"0:hello:", "1:sure?:", "2:name?:default", "2:cancelled?:x"};
    require(seen == expected, "The injection did not see the expected dialogs");
    return 0;
}

int rejectMta() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;
    bool rejected = false;
    try {
        webfront::testdriver::QuietFrontend<webfront::frontend::WebView2Frontend>::open("9002", "index.html");
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find("STA") != std::string_view::npos;
    }
    CoUninitialize();
    return rejected ? 0 : 1;
}

class SlowPageFS {
public:
    static inline std::atomic<bool>          waiting{false};
    static inline std::counting_semaphore<2> release{0};
    explicit SlowPageFS(std::filesystem::path root) : source(std::move(root)) {}
    std::optional<webfront::fs::File> open(std::filesystem::path file) {
        if (file.relative_path() == "slow-resource.js") {
            waiting = true;
            require(release.try_acquire_for(std::chrono::seconds{10}), "The slow-resource fixture was never released");
        }
        return source.open(file);
    }

private:
    webfront::fs::NativeDebugFS source;
};

int runtimeReadyBeforePageLoad() {
    using namespace webfront::frontend::detail::webview2;
    webfront::http::Server<webfront::NetProvider, SlowPageFS> server("127.0.0.1", "9002", std::filesystem::current_path());
    std::jthread                                              worker([&](std::stop_token stop) {
        std::stop_callback cleanup(stop, [&] {
            SlowPageFS::release.release();
            server.stop();
        });
        server.run();
    });
    Apartment                                                 apartment;
    auto                                                      session = std::make_shared<Session>();
    session->url                                                      = L"http://127.0.0.1:9002/slow-page.html";
    session->createWindow(webfront::testdriver::diagnosticWindows());
    require(static_cast<bool>(IsWindowVisible(session->window)) == webfront::testdriver::diagnosticWindows(), "Window visibility option was ignored");
    const auto oldTimer = session->startupTimer;
    Actions    actions;
    bool       observed      = false;
    bool       closeObserved = false;
    actions.onFailure        = [&] {
        session->fail();
    };
    actions.dispatch = [&](UINT) {
        if (!SlowPageFS::waiting)
            return;
        KillTimer(actions.window, 1);
        require(session->loadingInitialPage(), "The fixture must still be loading its first page");
        require(!session->startupTimer, "Runtime watchdog still covers page loading");
        EventRegistrationToken token{};
        check(session->view->add_WindowCloseRequested(
                  Microsoft::WRL::Callback<ICoreWebView2WindowCloseRequestedEventHandler>([&](ICoreWebView2*, IUnknown*) -> HRESULT {
                      closeObserved = true;
                      return S_OK;
                  }).Get(),
                  &token),
              "observing slow-page closure");
        observed = true;
        PostThreadMessageW(GetCurrentThreadId(), WM_TIMER, oldTimer, 0);
        SlowPageFS::release.release();
    };
    require(SetTimer(actions.window, 1, 10, nullptr) != 0, "Cannot start page-loading observer");
    session->start(userDataFolder());
    session->run();
    require(observed, "The slow page was not observed before navigation completed");
    require(closeObserved, "The initial page did not request normal closure");
    return 0;
}

int expectOpenFailure(std::string_view port, std::string_view diagnostic) {
    try {
        webfront::testdriver::QuietFrontend<webfront::frontend::WebView2Frontend>::open(port, "index.html");
    } catch (const std::exception& error) {
        return std::string_view(error.what()).find(diagnostic) != std::string_view::npos ? 0 : 1;
    }
    return 1;
}

class EarlyNativeClose {
public:
    bool closeSent = false;

    explicit EarlyNativeClose(bool enabled) {
        if (!enabled)
            return;
        current = this;
        hook    = SetWindowsHookExW(WH_CBT, onWindowEvent, nullptr, GetCurrentThreadId());
        require(hook != nullptr, "Cannot observe native window creation");
    }
    ~EarlyNativeClose() {
        if (hook)
            UnhookWindowsHookEx(hook);
        current = nullptr;
    }

private:
    HHOOK                                        hook    = nullptr;
    static inline thread_local EarlyNativeClose* current = nullptr;

    static LRESULT CALLBACK onWindowEvent(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HCBT_CREATEWND && current) {
            const auto window = reinterpret_cast<HWND>(wParam);
            wchar_t    name[80]{};
            GetClassNameW(window, name, 80);
            if (std::wstring_view(name).starts_with(L"WebFront.WebView2."))
                current->closeSent = PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
};

bool comReleased() {
    APTTYPE          apartment{};
    APTTYPEQUALIFIER qualifier{};
    const auto       result = CoGetApartmentType(&apartment, &qualifier);
    if (result == CO_E_NOTINITIALIZED)
        return true;
    if (result != S_OK)
        return false;
    // WebView2 workers may retain a process-wide implicit MTA, but our STA must be released.
    return apartment == APTTYPE_MTA && qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA;
}

int showTwice(std::string_view page, bool earlyClose = false) {
    // Repeated show() checks that WM_QUIT cannot poison the next loop.
    for (int iteration = 0; iteration < 2; ++iteration) {
        App               app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
        std::atomic<bool> connected = false;
        app.onUIStarted([&](App::UI) {
            connected = true;
        });
        EarlyNativeClose closer(earlyClose);
        app.show(page);
        if (earlyClose)
            require(closer.closeSent, "Native close was not sent during window creation");
        require(!connected, "Unexpected bridge connection");
        require(comReleased(), "STA COM initialization was not released");
    }
    return 0;
}

int run(std::string_view mode) {
    if (!webView2ClosesServer())
        return 1;
    const std::map<std::string_view, std::function<int()>> cases{
        {            "file-choose",
         [] {
         return fileServices("choose");
         }                                                    },
        {            "file-cancel",
         [] {
         return fileServices("cancel");
         }                                                    },
        {         "script-dialogs", scriptDialogs},
        {          "file-download",
         [] {
         return fileServices("download");
         }                                                    },
        {          "runtime-ready", runtimeReadyBeforePageLoad},
        { "closed-startup-timeout",
         [] {
         return startupCompletion(false);
         }                                                    },
        { "failed-startup-timeout",
         [] {
         return startupCompletion(true);
         }                                                    },
        {"late-navigation-failure",      lateNavigationFailure},
        {                    "mta",                  rejectMta},
        {        "missing-runtime",
         [] {
         SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", L"C:\\WebFront-runtime-does-not-exist");
         return expectOpenFailure("9002", "Evergreen Runtime");
         }                                                    },
        {          "repeated-show",
         [] {
         showTwice("rejected-ready.html");
         showTwice("encoded path %23.html?x=1#example");
         return showTwice("no-bridge.html");
         }                                                    },
        {  "navigation-diagnostic",
         [] {
         return expectOpenFailure("0", "navigation failed");
         }                                                    },
        {     "native-early-close",
         [] {
         return showTwice("no-bridge.html", true);
         }                                                    },
    };
    const auto test = cases.find(mode);
    return test == cases.end() ? 1 : test->second();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return argc == 2 ? run(argv[1]) : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
