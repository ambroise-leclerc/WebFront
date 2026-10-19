#include <system/NativeFS.hpp>
#include <WebFront.hpp>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

#ifndef WEBFRONT_TEST_FRONTEND
    #error WEBFRONT_TEST_FRONTEND must select an embedded frontend
#endif

namespace {
using Frontend = WEBFRONT_TEST_FRONTEND;
using TestFS   = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
using App      = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, Frontend>;
static_assert(Frontend::action == webfront::frontend::Action::closeServerAfterOpen);

int show(std::string_view page) {
    App               app("9002", std::filesystem::current_path());
    std::atomic<bool> connected = false;
    app.onUIStarted([&](App::UI) {
        connected = true;
    });
    app.show(page);
    if (connected)
        throw std::runtime_error("Unexpected bridge connection");
    return 0;
}

int expectNavigationFailure() {
    try {
        Frontend::open("0", "index.html");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 0;  // The contract requires an exception, without prescribing its wording.
    }
    throw std::runtime_error("Failed initial navigation did not throw");
}

// Report the engine's actual cancellation to the surviving document. Chromium does not
// consistently expose embedder cancellation through the JavaScript Navigation API.
constexpr auto cancellationScript = "globalThis.dispatchEvent(new Event('webfront-policy-cancelled'))";

#ifdef WEBFRONT_EMBED_CEF
class PolicyLifecycleClient : public webfront::cef::SimpleCEFClient {
public:
    using SimpleCEFClient::SimpleCEFClient;
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool gesture, bool redirect) override {
        const bool cancelled = SimpleCEFClient::OnBeforeBrowse(browser, frame, request, gesture, redirect);
        if (cancelled && frame->IsMain())
            frame->ExecuteJavaScript(cancellationScript, frame->GetURL(), 0);
        return cancelled;
    }
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser>         browser,
                          CefRefPtr<CefFrame>           frame,
                          const CefString&              target,
                          cef_window_open_disposition_t disposition,
                          bool                          gesture) override {
        const bool cancelled = SimpleCEFClient::OnOpenURLFromTab(browser, frame, target, disposition, gesture);
        if (cancelled && disposition == CEF_WOD_CURRENT_TAB && frame && frame->IsMain())
            frame->ExecuteJavaScript(cancellationScript, frame->GetURL(), 0);
        return cancelled;
    }
};
#endif

struct PolicyLifecycleFrontend {
    static constexpr auto action = Frontend::action;
    static void           initialize() {
        webfront::detail::ensureFrontendInitialized<Frontend>();
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const webfront::ContentPolicy& policy) {
#ifdef WEBFRONT_EMBED_CEF
        if constexpr (std::same_as<Frontend, webfront::frontend::CEFFrontend>) {
            webfront::cef::detail::openWindow(webfront::frontend::detail::applicationUrl(address, port, page),
                                              webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                                              new PolicyLifecycleClient);
        } else
#endif
#if defined(WEBFRONT_EMBED_WEBVIEW2) && defined(_WIN32)
            if constexpr (std::same_as<Frontend, webfront::frontend::WebView2Frontend>) {
            using namespace webfront::frontend::detail::webview2;
            Apartment apartment;
            auto      session            = std::make_shared<Session>();
            session->contentPolicy       = webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy};
            session->url                 = applicationUrl(address, port, page);
            session->navigationCancelled = [weak = std::weak_ptr<Session>(session)] {
                if (auto live = weak.lock())
                    check(live->view->ExecuteScript(utf16(cancellationScript).c_str(), nullptr), "reporting native navigation cancellation");
            };
            session->createWindow();
            session->start(userDataFolder());
            session->run();
        } else
#endif
#if defined(WEBFRONT_EMBED_WKWEBVIEW) && defined(__APPLE__)
        if constexpr (std::same_as<Frontend, webfront::frontend::WKWebViewFrontend>) {
            using namespace webfront::frontend::detail::wkwebview;
            initialize();
            Object pool(send(type("NSAutoreleasePool"), "new"));
            Session session(webfront::frontend::detail::applicationUrl(address, port, page), {.contentPolicy = policy});
            session.navigationCancelled = [&] {
                Object script(text(cancellationScript));
                send<void>(session.view, "evaluateJavaScript:completionHandler:", script.get(), static_cast<id>(nullptr));
            };
            session.create();
            session.start();
            session.run();
        } else
#endif
        {
            throw std::runtime_error("Content-policy lifecycle probe needs a native cancellation observer for this frontend");
        }
    }
};

int checkContentPolicy(bool allowNavigation) {
    webfront::ContentPolicy policy;
    policy.bridgeAllowedOrigins = {"http://localhost:9002"};
    if (allowNavigation)
        policy.navigationAllowedOrigins = {"http://localhost:9002"};
    using PolicyApp = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, PolicyLifecycleFrontend>;
    PolicyApp         app(webfront::WebFrontConfig{.address = "0.0.0.0", .documentRoot = std::filesystem::current_path(), .contentPolicy = policy});
    std::atomic<bool> completed = false;
    std::atomic<bool> escaped   = false;
    std::atomic<bool> navigated = false;
    app.cppFunction("policyCompleted", [&] {
        completed = true;
    });
    app.cppFunction("policyEscaped", [&] {
        escaped = true;
    });
    app.cppFunction("policyNavigated", [&] {
        navigated = true;
    });
    app.show("content-policy.html");
    if (escaped)
        throw std::runtime_error("Frontend allowed an unmanaged popup");
    if (completed == allowNavigation)
        throw std::runtime_error("Frontend reported an unexpected navigation cancellation");
    if (navigated != allowNavigation)
        throw std::runtime_error("Frontend did not reach the expected destination");
    return 0;
}

int run(std::string_view mode) {
    std::map<std::string_view, int (*)()> cases;
    cases.emplace(
        "content-policy",
        +[] {
            return checkContentPolicy(false);
        });
    cases.emplace(
        "navigation-allow-list",
        +[] {
            return checkContentPolicy(true);
        });
    cases.emplace(
        "no-bridge",
        +[] {
            return show("no-bridge.html");
        });
    cases.emplace(
        "early-close",
        +[] {
            return show("no-bridge.html?early-close");
        });
    cases.emplace(
        "rejected-ready",
        +[] {
            return show("rejected-ready.html");
        });
    cases.emplace(
        "path-encoding",
        +[] {
            return show("encoded path %23.html?x=1#example");
        });
    cases.emplace("navigation-failure", expectNavigationFailure);
    const auto selected = cases.find(mode);
    if (selected == cases.end())
        throw std::runtime_error("Unknown lifecycle scenario");
    return selected->second();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        // CEF subprocesses have their own argument list and must dispatch before mode parsing.
        webfront::detail::ensureFrontendInitialized<Frontend>();
        return argc == 2 ? run(argv[1]) : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
