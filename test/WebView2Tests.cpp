#include "WebView2ControllerMock.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <frontend/WebView2.hpp>

SCENARIO("WebView2 encodes application paths") {
    using webfront::frontend::detail::webview2::applicationUrl;
    GIVEN("a UTF-8 filename containing URL delimiters") {
        THEN("only path separators and unreserved characters remain literal") {
            REQUIRE(applicationUrl("9002", "folder/a b#c?d%\xc3\xa9.html") == L"http://localhost:9002/folder/a%20b%23c%3Fd%25%C3%A9.html");
        }
    }
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
