#include "WebView2ControllerMock.hpp"
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
