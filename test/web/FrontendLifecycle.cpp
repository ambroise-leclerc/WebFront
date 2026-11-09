#include "../driver/TestFrontend.hpp"
#include <webfront/system/NativeFS.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <thread>
#include <filesystem>
#include <iostream>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifndef WEBFRONT_TEST_FRONTEND
    #error WEBFRONT_TEST_FRONTEND must select an embedded frontend
#endif

namespace {
using Frontend = WEBFRONT_TEST_FRONTEND;
using TestFS   = webfront::fs::Multi<webfront::fs::IndexFS, webfront::fs::NativeDebugFS>;
using App      = webfront::BasicWFWithFrontend<webfront::NetProvider, TestFS, webfront::testdriver::QuietFrontend<Frontend>>;
static_assert(Frontend::action == webfront::frontend::Action::closeServerAfterOpen);

int show(std::string_view page) {
    App               app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    std::atomic<bool> connected = false;
    app.onUIStarted([&](App::UI) {
        connected = true;
    });
    app.show(page);
    if (connected)
        throw std::runtime_error("Unexpected bridge connection");
    return 0;
}

// The page of the view is associated with the view of the frontend; a second connection from the same page, without the
// secret, is not (ADR-020, rule 3).
int checkViewAssociation() {
    App                 app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    std::mutex          mutex;
    std::vector<bool>   embedded;
    std::vector<bool>   local;
    std::atomic<bool>   originsChecked = false;
    app.onUIStarted([&](App::UI ui) {
        std::scoped_lock lock{mutex};
        embedded.push_back(ui.isEmbeddedView());
        local.push_back(ui.isLocalClient());
    });
    app.cppFunction("startedConnections", [&] {
        std::scoped_lock lock{mutex};
        return static_cast<std::uint32_t>(embedded.size());
    });
    app.cppFunction("associationScript", [](std::string address) {
        return webfront::detail::viewSecretScript(std::string(64, 'a'), webfront::detail::browserOrigin(address, "9002"));
    });
    app.cppFunction("originsChecked", [&] { originsChecked = true; });
    app.show("view-association.html");
    if (!originsChecked)
        throw std::runtime_error("The view association script did not match the browser's serialized IPv6 origins");
    std::scoped_lock lock{mutex};
    if (embedded.size() != 2)
        throw std::runtime_error("Expected two bridge connections");
    if (!embedded[0] || !local[0])
        throw std::runtime_error("The page of the view is not associated with the view");
    if (embedded[1] || local[1])
        throw std::runtime_error("A connection without the secret was associated with the view");
    return 0;
}

// What the interface thread's executor reports for one operation.
enum class Outcome { ran, stopped, unavailable, failed };

struct OutcomeReceiver {
    using receiver_concept = webfront::networking::ex::receiver_tag;
    std::promise<std::pair<Outcome, bool>>* result;  // the Outcome, and whether it completed on the interface thread
    webfront::frontend::UIExecutor          executor;
    auto                                    get_env() const noexcept {
        return webfront::networking::ex::prop{webfront::networking::ex::get_stop_token, webfront::networking::ex::never_stop_token{}};
    }
    void set_value() && noexcept {
        result->set_value({Outcome::ran, executor.onUIThread()});
    }
    void set_stopped() && noexcept {
        result->set_value({Outcome::stopped, false});
    }
    void set_error(std::exception_ptr error) && noexcept {
        try {
            std::rethrow_exception(error);
        } catch (const webfront::frontend::UIExecutorUnavailable&) {
            result->set_value({Outcome::unavailable, false});
        } catch (...) {
            result->set_value({Outcome::failed, false});
        }
    }
};

struct ScheduledOperation {
    std::promise<std::pair<Outcome, bool>> result;
    decltype(webfront::networking::ex::connect(std::declval<decltype(std::declval<webfront::frontend::UIExecutor>().schedule())>(),
                                              std::declval<OutcomeReceiver>()))
        operation;
    explicit ScheduledOperation(const webfront::frontend::UIExecutor& executor)
        : operation(webfront::networking::ex::connect(executor.schedule(), OutcomeReceiver{&result, executor})) { }
    std::future<std::pair<Outcome, bool>> start() {
        auto future = result.get_future();
        webfront::networking::ex::start(operation);
        return future;
    }
};

// The window's interface thread runs the operations, and closing the window completes the pending ones stopped: every
// operation completes exactly once, none runs on another thread or after closure, and the executor is unavailable afterwards.
int checkUIExecutor() {
    App         app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    const auto  executor = app.uiExecutor();
    const auto  mainThread = std::this_thread::get_id();
    if (executor.available())
        throw std::runtime_error("The executor is available before the window opens");
    std::atomic<int>  ran = 0;
    std::atomic<bool> error = false;
    std::string       message;
    std::jthread      client([&] {
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!executor.available()) {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("The executor never became available");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::list<ScheduledOperation>                       operations;
            std::vector<std::future<std::pair<Outcome, bool>>> results;
            while (executor.available() && results.size() < 5000) {
                operations.emplace_back(executor);
                results.push_back(operations.back().start());
                if (results.size() % 16 == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (auto& future : results) {
                if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
                    throw std::runtime_error("An operation never completed");
                const auto [outcome, onInterfaceThread] = future.get();
                if (outcome == Outcome::ran) {
                    ++ran;
                    if (!onInterfaceThread)
                        throw std::runtime_error("An operation ran away from the interface thread");
                } else if (outcome == Outcome::failed) {
                    throw std::runtime_error("An operation failed");
                }
            }
        } catch (const std::exception& e) {
            message = e.what();
            error   = true;
        }
    });
    app.show("no-bridge.html");
    client.join();
    if (error)
        throw std::runtime_error(message);
    if (ran == 0)
        throw std::runtime_error("No operation ran on the interface thread");
    if (executor.available() || executor.onUIThread())
        throw std::runtime_error("The executor is still available after the window closed");
    ScheduledOperation late(executor);
    auto               future = late.start();
    if (future.get().first != Outcome::unavailable)
        throw std::runtime_error("An operation scheduled after closure was not refused");
    (void)mainThread;
    return 0;
}

// Reads the native window on the interface thread, from an operation of the executor.
struct WindowReader {
    using receiver_concept = webfront::networking::ex::receiver_tag;
    std::promise<webfront::frontend::NativeWindowResult>* result;
    webfront::frontend::NativeWindow                      window;
    auto                                                  get_env() const noexcept {
        return webfront::networking::ex::prop{webfront::networking::ex::get_stop_token, webfront::networking::ex::never_stop_token{}};
    }
    void set_value() && noexcept {
        result->set_value(window.get());
    }
    void set_stopped() && noexcept {
        result->set_value(std::unexpected(webfront::frontend::NativeWindowError::closed));
    }
    void set_error(std::exception_ptr) && noexcept {
        result->set_value(std::unexpected(webfront::frontend::NativeWindowError::unsupported));
    }
};

// The native window of the frontend: absent before open, refused to other threads, given on the interface thread while the
// window lives, and gone once it closed.
int checkNativeWindow() {
    using webfront::frontend::NativeWindowError;
    App        app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path()});
    const auto window   = app.nativeWindow();
    const auto executor = app.uiExecutor();
    if (window.get().error_or(NativeWindowError::closed) != NativeWindowError::notOpen)
        throw std::runtime_error("The window is given (or has a wrong error) before the window opens");
    std::atomic<bool> error = false;
    std::string       message;
    std::jthread      client([&] {
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            // CEF can publish the window (OnAfterCreated) while it creates it, before open() attaches the executor: wait for both,
            // or the read scheduled below could find no loop.
            while (window.get().error_or(NativeWindowError::closed) == NativeWindowError::notOpen || !executor.available()) {
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("The window was never published, or the executor never became available");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (window.get().error_or(NativeWindowError::notOpen) != NativeWindowError::wrongThread)
                throw std::runtime_error("The window was given to a thread that is not the interface thread");
            std::promise<webfront::frontend::NativeWindowResult> answer;
            auto operation = webfront::networking::ex::connect(executor.schedule(), WindowReader{&answer, window});
            auto future    = answer.get_future();
            webfront::networking::ex::start(operation);
            if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
                throw std::runtime_error("The read on the interface thread never completed");
            const auto read = future.get();
            if (!read)
                throw std::runtime_error("The window was not given on the interface thread");
            if (!read->pointer)
                throw std::runtime_error("The native window is a null pointer");
        } catch (const std::exception& e) {
            message = e.what();
            error   = true;
        }
    });
    app.show("no-bridge.html");
    client.join();
    if (error)
        throw std::runtime_error(message);
    if (window.get().error_or(NativeWindowError::notOpen) != NativeWindowError::closed)
        throw std::runtime_error("The window is still given after the window closed");
    return 0;
}

int expectNavigationFailure() {
    try {
        webfront::testdriver::QuietFrontend<Frontend>::open("0", "index.html");
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
#if defined(WEBFRONT_EMBED_WEBKITGTK) && defined(__linux__)
        if constexpr (std::same_as<Frontend, webfront::frontend::WebKitGTKFrontend>) {
            webfront::frontend::detail::webkitgtk::OpenOptions options{.contentPolicy = policy};
            options.configure = [](auto& session) {
                session.navigationCancelled = [&session] {
                    session.evaluate(cancellationScript);
                };
            };
            webfront::frontend::detail::webkitgtk::open(address, port, page, options);
            return;
        }
#endif
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
            session->createWindow(webfront::testdriver::diagnosticWindows());
            session->start(userDataFolder());
            session->run();
        } else
#endif
#if defined(WEBFRONT_EMBED_WKWEBVIEW) && defined(__APPLE__)
        if constexpr (std::same_as<Frontend, webfront::frontend::WKWebViewFrontend>) {
            using namespace webfront::frontend::detail::wkwebview;
            initialize();
            Object  pool(send(type("NSAutoreleasePool"), "new"));
            Session session(webfront::frontend::detail::applicationUrl(address, port, page),
                            {.contentPolicy = policy, .visible = webfront::testdriver::diagnosticWindows()});
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
    PolicyApp         app(webfront::WebFrontConfig{.documentRoot = std::filesystem::current_path(), .contentPolicy = policy});
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
    cases.emplace("ui-executor", checkUIExecutor);
    cases.emplace("native-window", checkNativeWindow);
    cases.emplace("view-association", checkViewAssociation);
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
