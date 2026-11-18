#include "WebView2ControllerMock.hpp"
#include "WebView2PolicyMocks.hpp"
#include "Bdd.hpp"
#include <webfront/frontend/WebView2.hpp>

#include <thread>

using namespace webfront::bdd;

namespace {
struct ViewRegistrationBrowser {
    Microsoft::WRL::ComPtr<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> completion;
    unsigned navigations = 0;
    HRESULT navigationResult = S_OK;
    HRESULT AddScriptToExecuteOnDocumentCreated(LPCWSTR, ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler* handler) {
        completion = handler;
        return S_OK;
    }
    HRESULT Navigate(LPCWSTR) {
        ++navigations;
        return navigationResult;
    }
};
}

const Scenario webView2Waits{"WebView2 waits for view association registration before navigation", [] {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto session = std::make_shared<Session>(std::chrono::milliseconds{25});
    session->createWindow();
    session->viewLaunch = {std::string(64, 'a'), "http://127.0.0.1:9002"};
    ViewRegistrationBrowser browser;
    session->installViewAssociationThenNavigate(&browser);
    require(browser.completion);
    require(browser.navigations == 0);
    require(session->startupTimer != 0);
    when("registration completes successfully", [&] {
        require(browser.completion->Invoke(S_OK, L"script") == S_OK);
        require(browser.navigations == 1);
        require(session->startupTimer == 0);
        require(!session->failure);
    });
    when("registration fails", [&] {
        require(browser.completion->Invoke(E_FAIL, nullptr) == S_OK);
        require(browser.navigations == 0);
        require(session->failure);
        require(session->startupTimer == 0);
    });
    when("registration never completes", [&] {
        requireThrows([&] { session->run(); }, contains("25 milliseconds"));
        require(browser.navigations == 0);
    });
    when("the window closes before registration completes", [&] {
        session->close();
        require(browser.completion->Invoke(S_OK, L"script") == S_OK);
        require(browser.navigations == 0);
    });
    when("navigation fails after successful registration", [&] {
        browser.navigationResult = E_FAIL;
        require(browser.completion->Invoke(S_OK, L"script") == S_OK);
        require(session->failure);
    });
}};

const Scenario webView2Installs{"WebView2 installs the window close script even without a view association secret", [] {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto session = std::make_shared<Session>();
    session->createWindow();
    ViewRegistrationBrowser browser;
    session->installViewAssociationThenNavigate(&browser);
    require(browser.completion);
    require(browser.navigations == 0);
    require(browser.completion->Invoke(S_OK, L"script") == S_OK);
    require(browser.navigations == 1);
    require(session->startupTimer == 0);
    then("the script asks the host to close instead of relying on Chromium's window.close() filter", [&] {
        const auto script = Session::windowCloseScript();
        require(script, contains("webfront:close-window"));
        require(script, contains("window.top!==window"));
        require(Session::windowCloseMessage() == L"webfront:close-window");
    });
}};

const Scenario webView2Encodes{"WebView2 encodes application paths", [] {
    using webfront::frontend::detail::webview2::applicationUrl;
    given("a UTF-8 filename containing percent-escaped URL delimiters", [&] {
        then("all frontends share URI-reference encoding", [&] {
            require(applicationUrl("9002", "folder/a b%23c%3Fd%25\xc3\xa9.html") == L"http://127.0.0.1:9002/folder/a%20b%23c%3Fd%25%C3%A9.html");
            require(applicationUrl("127.0.0.2", "9002", "index.html?x=1#result") == L"http://127.0.0.2:9002/index.html?x=1#result");
        });
    });
}};

const Scenario webView2Presentation{"WebView2 prepares a background and a first-frame probe", [] {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto session = std::make_shared<Session>();
    webfront::WindowOptions options;
    options.background = "#123456";
    session->createWindow(false, options);
    const auto brush = reinterpret_cast<HBRUSH>(GetClassLongPtrW(session->window, GCLP_HBRBACKGROUND));
    LOGBRUSH description{};
    require(GetObjectW(brush, sizeof(description), &description) == static_cast<int>(sizeof(description)));
    require(description.lbColor == RGB(0x12, 0x34, 0x56));
    const auto script = Session::firstFrameScript();
    require(script, contains("window.top!==window"));
    require(script, contains("DOMContentLoaded"));
    require(script, contains("webfront:first-frame"));
    require(script, contains("requestAnimationFrame(()=>requestAnimationFrame"));
}};

const Scenario webView2PresentationDeadline{"WebView2 reveals a page that never reports its first frame", [] {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto session = std::make_shared<Session>();
    session->createWindow();
    BYTE initial = 0;
    require(GetLayeredWindowAttributes(session->window, nullptr, &initial, nullptr));
    require(initial == 3);
    ViewRegistrationBrowser browser;
    session->installViewAssociationThenNavigate(&browser);
    require(browser.completion->Invoke(S_OK, L"script") == S_OK);
    BYTE finalAlpha = 0;
    const HWND window = session->window;
    std::jthread observer([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            GetLayeredWindowAttributes(window, nullptr, &finalAlpha, nullptr);
        } while (finalAlpha != 255 && std::chrono::steady_clock::now() < deadline);
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    session->run();
    observer.join();
    require(finalAlpha == 255);
}};

const Scenario webView2EngineSlowToStart{"WebView2 shows its window after the delay even when the engine never starts", [] {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto session = std::make_shared<Session>();
    session->createWindow();  // no environment, no controller, no navigation: the engine is still starting
    BYTE initial = 0;
    require(GetLayeredWindowAttributes(session->window, nullptr, &initial, nullptr));
    require(initial == 3);
    BYTE finalAlpha = 0;
    const HWND window = session->window;
    std::jthread observer([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            GetLayeredWindowAttributes(window, nullptr, &finalAlpha, nullptr);
        } while (finalAlpha != 255 && std::chrono::steady_clock::now() < deadline);
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    session->run();
    observer.join();
    require(finalAlpha == 255);
}};

const Scenario webView2Runtime{"WebView2 runtime startup has a configurable deadline", [] {
    using namespace webfront::frontend::detail::webview2;
    using namespace std::chrono_literals;
    requireNothrow([&] { Session{3min}; });
    requireThrows<std::invalid_argument>([&] { Session{0ms}; });
    requireThrows<std::invalid_argument>([&] { Session{-1ms}; });
    Apartment apartment;
    auto      session = std::make_shared<Session>(25ms);
    session->createWindow();
    requireThrows([&] { session->run(); }, contains("25 milliseconds"));
}};

const Scenario webView2Controller{"WebView2 controller closure is idempotent and preserves errors", [] {
    using webfront::frontend::detail::webview2::Session;
    auto session            = std::make_shared<Session>();
    auto controller         = Microsoft::WRL::Make<webfront::test::WebView2ControllerMock>();
    session->controller     = controller;
    controller->duringClose = [&] {
        session->close();
    };
    controller->closeResult = E_FAIL;
    session->close();
    session->close();
    require(controller->closeCalls == 1);
    require(!session->controller);
    require(session->failure);
    requireThrows([&] { std::rethrow_exception(session->failure); }, contains("closing controller"));
}};

const Scenario webView2Tolerates{"WebView2 tolerates a temporarily unresponsive renderer", [] {
    using webfront::frontend::detail::webview2::Session;
    then("unresponsive and recoverable auxiliary processes are not fatal", [&] {
        requireNothrow([&] { Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE); });
        requireNothrow([&] { Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_GPU_PROCESS_EXITED); });
    });
    then("browser and main renderer exits remain fatal", [&] {
        requireThrows<std::runtime_error>([&] { Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED); });
        requireThrows<std::runtime_error>([&] { Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED); });
    });
}};

const Scenario webView2Errors{"WebView2 errors display the Windows HRESULT in hexadecimal", [] {
    using webfront::frontend::detail::webview2::check;
    then("the original HRESULT can be compared directly with Windows diagnostics", [&] {
        requireThrows([&] { check(HRESULT_FROM_WIN32(ERROR_BAD_COMPRESSION_BUFFER), "creating environment"); }, contains("HRESULT 0x8007025D"));
        requireNothrow([&] { check(S_OK, "creating environment"); });
    });
}};

const Scenario webView2Forwards{"WebView2 forwards native focus and movement messages", [] {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto      session = std::make_shared<Session>();
    session->createWindow();
    auto controller     = Microsoft::WRL::Make<webfront::test::WebView2ControllerMock>();
    session->controller = controller;
    when("the native window receives focus", [&] {
        SendMessageW(session->window, WM_SETFOCUS, 0, 0);
        then("focus is restored programmatically to the WebView", [&] {
            require(controller->focusCalls == 1);
            require(controller->focusReason == COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        });
    });
    when("the parent window moves", [&] {
        SendMessageW(session->window, WM_MOVE, 0, 0);
        then("the WebView is notified independently of resizing", [&] {
            require(controller->positionCalls == 1);
        });
    });
    when("Windows suggests new bounds after a DPI change", [&] {
        RECT suggested{100, 100, 1000, 800};
        SendMessageW(session->window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
        RECT actual{};
        require(GetWindowRect(session->window, &actual));
        then("the native window adopts the suggested size", [&] {
            require(actual.right - actual.left == 900);
            require(actual.bottom - actual.top == 700);
            require(!session->failure);
        });
    });
    when("controller shutdown sends reentrant window messages", [&] {
        controller->duringClose = [&] {
            controller->focusResult    = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            controller->positionResult = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            SendMessageW(session->window, WM_SETFOCUS, 0, 0);
            SendMessageW(session->window, WM_MOVE, 0, 0);
        };
        session->close();
        then("closed controllers are not called and closure remains successful", [&] {
            require(controller->focusCalls == 0);
            require(controller->positionCalls == 0);
            require(!session->failure);
        });
    });
    then("the window has a background while the runtime starts", [&] {
        require(GetClassLongPtrW(session->window, GCLP_HBRBACKGROUND) != 0);
    });
}};

const Scenario webView2Only{"WebView2 only forwards user-initiated popups", [] {
    using webfront::frontend::detail::webview2::Session;
    auto session           = std::make_shared<Session>();
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {.popups = webfront::PopupPolicy::systemBrowser}};
    auto args              = Microsoft::WRL::Make<webfront::test::WebView2PopupMock>();
    for (const BOOL initiated : {FALSE, TRUE}) {
        when((initiated ? "the user requests a popup" : "a script requests a popup without user initiation"), [&] {
            static bool launched = false;
            launched             = false;
            args->userInitiated  = initiated;
            session->newWindowRequested(args.Get(), [](const std::string&) {
                launched = true;
            });
            then("only the user-initiated popup reaches the external launcher", [&] {
                require(launched == (initiated != FALSE));
                require(args->handled);
                require(!session->failure);
            });
        });
    }
}};

const Scenario webView2Popup{"WebView2 popup launcher failures leave the application running", [] {
    using webfront::frontend::detail::webview2::Session;
    auto session           = std::make_shared<Session>();
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {.popups = webfront::PopupPolicy::systemBrowser}};
    auto args              = Microsoft::WRL::Make<webfront::test::WebView2PopupMock>();
    when("the external launcher throws a standard exception", [&] {
        session->newWindowRequested(args.Get(), [](const std::string&) {
            throw std::runtime_error("No URL association");
        });
        require(args->handled);
        require(!session->failure);
        require(!session->closed);
    });
    when("the external launcher throws a non-standard exception", [&] {
        session->newWindowRequested(args.Get(), [](const std::string&) {
            throw 42;
        });
        require(args->handled);
        require(!session->failure);
        require(!session->closed);
    });
    when("marking the popup handled fails", [&] {
        args->handledResult = E_FAIL;
        session->newWindowRequested(args.Get());
        require(session->failure);
    });
    when("reading the popup URI fails", [&] {
        args->uriResult = E_FAIL;
        session->newWindowRequested(args.Get());
        require(session->failure);
    });
    when("reading popup user initiation fails", [&] {
        args->userInitiatedResult = E_FAIL;
        session->newWindowRequested(args.Get());
        require(session->failure);
    });
}};

const Scenario webView2Content{"WebView2 content controls map to engine settings", [] {
    auto settings = Microsoft::WRL::Make<webfront::test::WebView2SettingsMock>();
    using webfront::frontend::detail::webview2::configureContentSettings;
    configureContentSettings(settings.Get(), {});
    require(settings->devTools);
    require(settings->contextMenus);
    configureContentSettings(settings.Get(), {.devToolsEnabled = false});
    require(!settings->devTools);
    require(settings->contextMenus);
    configureContentSettings(settings.Get(), {.contextMenusEnabled = false});
    require(settings->devTools);
    require(!settings->contextMenus);
    settings->settingsResult = E_FAIL;
    requireThrows<std::runtime_error>([&] { configureContentSettings(settings.Get(), {}); });
}};

const Scenario apartmentWebView2{"The apartment of a WebView2 window leaves the thread as it found it", [] {
    using namespace webfront::frontend::detail::webview2;
    // A thread of its own: no other scenario and no engine has initialized COM there (#564).
    struct State {
        HRESULT          result{};
        APTTYPE          apartment{};
        APTTYPEQUALIFIER qualifier{};
        bool             operator==(const State&) const = default;
        static State     read() {
            State state;
            state.result = CoGetApartmentType(&state.apartment, &state.qualifier);
            return state;
        }
    };
    State before, during, after;
    std::thread([&] {
        before = State::read();
        {
            Apartment apartment;
            during = State::read();
        }
        after = State::read();
    }).join();
    require(during.result == S_OK);
    require((during.apartment == APTTYPE_STA || during.apartment == APTTYPE_MAINSTA));
    require(after == before);
}};
