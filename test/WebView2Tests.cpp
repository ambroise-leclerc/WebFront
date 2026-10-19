#include "WebView2ControllerMock.hpp"
#include "WebView2PolicyMocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <frontend/WebView2.hpp>

SCENARIO("WebView2 encodes application paths") {
    using webfront::frontend::detail::webview2::applicationUrl;
    GIVEN("a UTF-8 filename containing percent-escaped URL delimiters") {
        THEN("all frontends share URI-reference encoding") {
            REQUIRE(applicationUrl("9002", "folder/a b%23c%3Fd%25\xc3\xa9.html") == L"http://127.0.0.1:9002/folder/a%20b%23c%3Fd%25%C3%A9.html");
            REQUIRE(applicationUrl("127.0.0.2", "9002", "index.html?x=1#result") == L"http://127.0.0.2:9002/index.html?x=1#result");
        }
    }
}

SCENARIO("WebView2 runtime startup has a configurable deadline") {
    using namespace webfront::frontend::detail::webview2;
    using namespace std::chrono_literals;
    REQUIRE_NOTHROW(Session{3min});
    REQUIRE_THROWS_AS(Session{0ms}, std::invalid_argument);
    REQUIRE_THROWS_AS(Session{-1ms}, std::invalid_argument);
    Apartment apartment;
    auto      session = std::make_shared<Session>(25ms);
    session->createWindow();
    REQUIRE_THROWS_WITH(session->run(), Catch::Matchers::ContainsSubstring("25 milliseconds"));
}

SCENARIO("WebView2 controller closure is idempotent and preserves errors") {
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
    REQUIRE(controller->closeCalls == 1);
    REQUIRE_FALSE(session->controller);
    REQUIRE(session->failure);
    REQUIRE_THROWS_WITH(std::rethrow_exception(session->failure), Catch::Matchers::ContainsSubstring("closing controller"));
}

SCENARIO("WebView2 tolerates a temporarily unresponsive renderer") {
    using webfront::frontend::detail::webview2::Session;
    THEN("unresponsive and recoverable auxiliary processes are not fatal") {
        REQUIRE_NOTHROW(Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE));
        REQUIRE_NOTHROW(Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_GPU_PROCESS_EXITED));
    }
    THEN("browser and main renderer exits remain fatal") {
        REQUIRE_THROWS_AS(Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED), std::runtime_error);
        REQUIRE_THROWS_AS(Session::checkProcessFailure(COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED), std::runtime_error);
    }
}

SCENARIO("WebView2 errors display the Windows HRESULT in hexadecimal") {
    using webfront::frontend::detail::webview2::check;
    THEN("the original HRESULT can be compared directly with Windows diagnostics") {
        REQUIRE_THROWS_WITH(check(HRESULT_FROM_WIN32(ERROR_BAD_COMPRESSION_BUFFER), "creating environment"),
                            Catch::Matchers::ContainsSubstring("HRESULT 0x8007025D"));
        REQUIRE_NOTHROW(check(S_OK, "creating environment"));
    }
}

SCENARIO("WebView2 forwards native focus and movement messages") {
    using namespace webfront::frontend::detail::webview2;
    Apartment apartment;
    auto      session = std::make_shared<Session>();
    session->createWindow();
    auto controller     = Microsoft::WRL::Make<webfront::test::WebView2ControllerMock>();
    session->controller = controller;
    WHEN("the native window receives focus") {
        SendMessageW(session->window, WM_SETFOCUS, 0, 0);
        THEN("focus is restored programmatically to the WebView") {
            REQUIRE(controller->focusCalls == 1);
            REQUIRE(controller->focusReason == COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    }
    WHEN("the parent window moves") {
        SendMessageW(session->window, WM_MOVE, 0, 0);
        THEN("the WebView is notified independently of resizing") {
            REQUIRE(controller->positionCalls == 1);
        }
    }
    WHEN("Windows suggests new bounds after a DPI change") {
        RECT suggested{100, 100, 1000, 800};
        SendMessageW(session->window, WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&suggested));
        RECT actual{};
        REQUIRE(GetWindowRect(session->window, &actual));
        THEN("the native window adopts the suggested size") {
            REQUIRE(actual.right - actual.left == 900);
            REQUIRE(actual.bottom - actual.top == 700);
            REQUIRE_FALSE(session->failure);
        }
    }
    WHEN("controller shutdown sends reentrant window messages") {
        controller->duringClose = [&] {
            controller->focusResult    = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            controller->positionResult = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
            SendMessageW(session->window, WM_SETFOCUS, 0, 0);
            SendMessageW(session->window, WM_MOVE, 0, 0);
        };
        session->close();
        THEN("closed controllers are not called and closure remains successful") {
            REQUIRE(controller->focusCalls == 0);
            REQUIRE(controller->positionCalls == 0);
            REQUIRE_FALSE(session->failure);
        }
    }
    THEN("the window has a background while the runtime starts") {
        REQUIRE(GetClassLongPtrW(session->window, GCLP_HBRBACKGROUND) != 0);
    }
}

SCENARIO("WebView2 only forwards user-initiated popups") {
    using webfront::frontend::detail::webview2::Session;
    auto session           = std::make_shared<Session>();
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {.popups = webfront::PopupPolicy::systemBrowser}};
    auto args              = Microsoft::WRL::Make<webfront::test::WebView2PopupMock>();
    for (const BOOL initiated : {FALSE, TRUE}) {
        WHEN((initiated ? "the user requests a popup" : "a script requests a popup without user initiation")) {
            static bool launched = false;
            launched             = false;
            args->userInitiated  = initiated;
            session->newWindowRequested(args.Get(), [](const std::string&) {
                launched = true;
            });
            THEN("only the user-initiated popup reaches the external launcher") {
                REQUIRE(launched == (initiated != FALSE));
                REQUIRE(args->handled);
                REQUIRE_FALSE(session->failure);
            }
        }
    }
}

SCENARIO("WebView2 popup launcher failures leave the application running") {
    using webfront::frontend::detail::webview2::Session;
    auto session           = std::make_shared<Session>();
    session->contentPolicy = webfront::detail::BoundContentPolicy{"http://127.0.0.1:9002", {.popups = webfront::PopupPolicy::systemBrowser}};
    auto args              = Microsoft::WRL::Make<webfront::test::WebView2PopupMock>();
    WHEN("the external launcher throws a standard exception") {
        session->newWindowRequested(args.Get(), [](const std::string&) {
            throw std::runtime_error("No URL association");
        });
        REQUIRE(args->handled);
        REQUIRE_FALSE(session->failure);
        REQUIRE_FALSE(session->closed);
    }
    WHEN("the external launcher throws a non-standard exception") {
        session->newWindowRequested(args.Get(), [](const std::string&) {
            throw 42;
        });
        REQUIRE(args->handled);
        REQUIRE_FALSE(session->failure);
        REQUIRE_FALSE(session->closed);
    }
    WHEN("marking the popup handled fails") {
        args->handledResult = E_FAIL;
        session->newWindowRequested(args.Get());
        REQUIRE(session->failure);
    }
    WHEN("reading the popup URI fails") {
        args->uriResult = E_FAIL;
        session->newWindowRequested(args.Get());
        REQUIRE(session->failure);
    }
    WHEN("reading popup user initiation fails") {
        args->userInitiatedResult = E_FAIL;
        session->newWindowRequested(args.Get());
        REQUIRE(session->failure);
    }
}

SCENARIO("WebView2 content controls map to engine settings") {
    auto settings = Microsoft::WRL::Make<webfront::test::WebView2SettingsMock>();
    using webfront::frontend::detail::webview2::configureContentSettings;
    configureContentSettings(settings.Get(), {});
    REQUIRE(settings->devTools);
    REQUIRE(settings->contextMenus);
    configureContentSettings(settings.Get(), {.devToolsEnabled = false});
    REQUIRE_FALSE(settings->devTools);
    REQUIRE(settings->contextMenus);
    configureContentSettings(settings.Get(), {.contextMenusEnabled = false});
    REQUIRE(settings->devTools);
    REQUIRE_FALSE(settings->contextMenus);
    settings->settingsResult = E_FAIL;
    REQUIRE_THROWS_AS(configureContentSettings(settings.Get(), {}), std::runtime_error);
}
