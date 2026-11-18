#include "Mocks.hpp"
#include "Bdd.hpp"
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/UIThread.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace webfront::bdd;
#ifndef _WIN32
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace {
using namespace webfront;
using namespace std::chrono_literals;
constexpr auto coordinationTimeout = 30s;

// Keep the server loop alive until stop(), unlike the immediate-return networking mock.
struct LoopState {
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    running         = false;
    bool                    stopped         = false;
    bool                    exited          = false;
    bool                    timedOut        = false;
    bool                    frontendEntered = false;
    bool                    windowCreated   = false;
    bool                    closeWindow     = false;
    bool                    showReturned    = false;
    std::thread::id         openThread;
};

class ContractIoContext {
public:
    static inline std::shared_ptr<LoopState> current;
    std::shared_ptr<LoopState>               state = std::make_shared<LoopState>();

    ContractIoContext() {
        current = state;
    }
    void run() {
        std::unique_lock lock(state->mutex);
        if (state->stopped) {
            state->exited = true;
            return;  // Destructor drain after the server thread has been joined.
        }
        state->running = true;
        state->changed.notify_all();
        state->timedOut = !state->changed.wait_for(lock, coordinationTimeout, [&] {
            return state->stopped;
        });
        state->running  = false;
        state->exited   = true;
    }
    std::size_t run_one() {
        return 0;
    }
    void stop() {
        std::lock_guard lock(state->mutex);
        state->stopped = true;
        state->changed.notify_all();
    }
    void restart() { }
};

struct ContractAcceptor : networking::SocketBaseMock {
    explicit ContractAcceptor(const ContractIoContext&) { }
    void open(auto) { }
    bool is_open() const {
        return true;
    }
};

struct ContractNet : networking::NetworkingMock {
    using IoContext = ContractIoContext;
    using Acceptor  = ContractAcceptor;
};

enum class Failure { none, standard, nonStandard, beforeWindow };

template <frontend::Action onReturn, Failure failure = Failure::none>
struct ContractFrontend {
    static constexpr frontend::Action action = onReturn;
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) {
        const auto       state = ContractIoContext::current;
        std::unique_lock lock(state->mutex);
        state->openThread      = std::this_thread::get_id();
        state->frontendEntered = true;
        state->changed.notify_all();
        if constexpr (failure == Failure::beforeWindow) {
            throw std::runtime_error("frontend failed before window creation");
        } else {
            state->windowCreated = true;
            if (!state->changed.wait_for(lock, coordinationTimeout, [&] {
                    return state->closeWindow;
                }))
                throw std::runtime_error("Mock window was not closed");
            if constexpr (failure == Failure::standard)
                throw std::runtime_error("frontend startup failed");
            if constexpr (failure == Failure::nonStandard)
                throw 42;
        }
    }
};

template <frontend::Action onReturn, Failure failure = Failure::none>
void checkSession() {
    using ContractApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, ContractFrontend<onReturn, failure>>;
    ContractApp                app;
    const auto         state = ContractIoContext::current;
    std::exception_ptr error;
    std::jthread       caller([&] {
        try {
            app.show();
        } catch (...) {
            error = std::current_exception();
        }
        std::lock_guard lock(state->mutex);
        state->showReturned = true;
    });
    {
        std::unique_lock lock(state->mutex);
        const bool       opened = state->changed.wait_for(lock, coordinationTimeout, [&] {
            return state->frontendEntered && state->running;
        });
        check(opened);
        check(state->openThread == caller.get_id());
        check(state->windowCreated);
        check(!state->showReturned);
        check(!state->stopped);
        // Release the simulated window before any fatal assertion or thread join.
        state->closeWindow = true;
        state->changed.notify_all();
    }
    caller.join();
    {
        std::lock_guard lock(state->mutex);
        if constexpr (onReturn == frontend::Action::closeServerAfterOpen || failure != Failure::none) {
            check(state->stopped);
            check(state->exited);
            check(!state->running);
        } else {
            check(!state->stopped);
            check(!state->exited);
            check(state->running);
        }
        check(!state->timedOut);
    }
    // Keep cleanup safe even if a CHECK above failed.
    app.stop();
    if constexpr (failure == Failure::none) {
        check(!error);
    } else {
        require(error);
        if constexpr (failure == Failure::standard)
            checkThrows([&] { std::rethrow_exception(error); }, equals("frontend startup failed"));
        else
            checkThrows<int>([&] { std::rethrow_exception(error); });
    }
}

template <frontend::Action onReturn>
void checkStartupFailure() {
    using ContractApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, ContractFrontend<onReturn, Failure::beforeWindow>>;
    ContractApp        app;
    const auto state = ContractIoContext::current;
    requireThrows([&] { app.show(); }, equals("frontend failed before window creation"));
    std::lock_guard lock(state->mutex);
    check(state->frontendEntered);
    check(!state->windowCreated);
    check(!state->closeWindow);
    check(state->stopped);
    check(state->exited);
    check(!state->running);
    check(!state->timedOut);
}
}  // namespace

const Scenario embeddedFrontend{"An embedded frontend blocks show until closure and completes the server loop", [] {
    given("a window held open while its server loop runs", [&] {
        when("show opens the window and the window is closed", [&] {
            then("show runs open on its calling thread and waits for shutdown after closure", [&] {
                checkSession<frontend::Action::closeServerAfterOpen>();
            });
        });
    });
}};

const Scenario launchingFrontend{"A launching frontend leaves the server loop running after show returns", [] {
    given("a frontend with keepServerRunning", [&] {
        when("show opens the frontend and open returns", [&] {
            then("the server remains active until explicitly stopped", [&] {
                checkSession<frontend::Action::keepServerRunning>();
            });
        });
    });
}};

const Scenario frontendExceptions{"Frontend exceptions propagate only after the server loop completes", [] {
    given("an embedded frontend that throws", [&] {
        when("open fails during window cleanup", [&] {
            then("the original diagnostic survives shutdown", [&] {
                checkSession<frontend::Action::closeServerAfterOpen, Failure::standard>();
            });
        });
    });
    given("a launching frontend that throws", [&] {
        when("open fails before returning", [&] {
            then("its action does not prevent shutdown", [&] {
                checkSession<frontend::Action::keepServerRunning, Failure::standard>();
            });
        });
    });
    given("a frontend throwing a non-standard exception", [&] {
        when("open throws during window cleanup", [&] {
            then("shutdown also precedes propagation of that exception", [&] {
                checkSession<frontend::Action::closeServerAfterOpen, Failure::nonStandard>();
            });
        });
    });
}};

const Scenario frontendStartup{"A frontend startup failure before window creation completes the server loop", [] {
    given("a frontend that fails before creating a window", [&] {
        when("show selects an embedded frontend", [&] {
            then("the server stops before the startup error propagates", [&] {
                checkStartupFailure<frontend::Action::closeServerAfterOpen>();
            });
        });
        when("show selects a launching frontend", [&] {
            then("the same shutdown guarantee holds", [&] {
                checkStartupFailure<frontend::Action::keepServerRunning>();
            });
        });
    });
}};

namespace {
struct WindowRecorder {
    static inline WindowOptions received;
    static constexpr frontend::Action action = frontend::Action::keepServerRunning;
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
    static void open(std::string_view, std::string_view, std::string_view, const ContentPolicy&, const WindowOptions& window) {
        received = window;
    }
};

WindowOptions windowReceivedBy(WebFrontConfig config) {
    using ContractApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, WindowRecorder>;
    WindowRecorder::received = {.title = "unset", .width = -1, .height = -1};
    ContractApp app(std::move(config));
    app.show();
    app.stop();
    return WindowRecorder::received;
}
}  // namespace

const Scenario windowBackground{"The window background is a CSS hexadecimal colour that is never guessed", [] {
    given("valid colours", [&] {
        then("short, long and translucent forms are read", [&] {
            const auto white = frontend::detail::parseBackground("#fff");
            const auto navy  = frontend::detail::parseBackground("#101c1d");
            const auto clear = frontend::detail::parseBackground("#00000080");
            require(white && navy && clear);
            check((*white)[0] == 1.0f && (*white)[3] == 1.0f);
            check((*navy)[0] == 16.0f / 255.0f && (*navy)[1] == 28.0f / 255.0f && (*navy)[2] == 29.0f / 255.0f && (*navy)[3] == 1.0f);
            check((*clear)[3] == 128.0f / 255.0f);
        });
    });
    given("invalid or empty text", [&] {
        then("nothing is returned, so the toolkit's default stays", [&] {
            check(!frontend::detail::parseBackground(""));
            check(!frontend::detail::parseBackground("red"));
            check(!frontend::detail::parseBackground("#12"));
            check(!frontend::detail::parseBackground("#12345"));
            check(!frontend::detail::parseBackground("#ggg"));
        });
    });
    given("a default configuration", [&] {
        then("the page keeps its media; an application without media opts out to skip GStreamer's initialization", [&] {
            check(WindowOptions{}.media);
            check(!windowReceivedBy({.window = {.media = false}}).media);
        });
    });
    given("a configuration with a background", [&] {
        const auto window = windowReceivedBy({.window = {.background = "#101c1d"}});
        then("the frontend receives it unchanged", [&] {
            check(window.background == "#101c1d");
        });
    });
}};

const Scenario applicationChooses{"The application chooses the title and the size of the embedded window", [] {
    given("a configuration with a window title and size", [&] {
        const auto window = windowReceivedBy({.window = {.title = "SiliconForge", .width = 640, .height = 480}});
        then("the frontend receives them unchanged", [&] {
            check(window.title == "SiliconForge");
            check(window.width == 640);
            check(window.height == 480);
        });
    });
    given("a default configuration", [&] {
        const auto window = windowReceivedBy({});
        then("the frontend receives an empty title and the default size", [&] {
            check(window.title.empty());
            check(window.width == 1200);
            check(window.height == 800);
        });
        then("the effective title is the executable's name, never the former constant", [&] {
            const auto effective = frontend::detail::resolveWindow(window);
            check(!effective.title.empty());
            check(effective.title != "WebFront Application");
        });
    });
    given("an invalid size and an explicit title", [&] {
        const auto effective = frontend::detail::resolveWindow({.title = "T", .width = 0, .height = -5});
        then("the title is kept and the size falls back to the default", [&] {
            check(effective.title == "T");
            check(effective.width == 1200);
            check(effective.height == 800);
        });
    });
}};

namespace {
struct LaunchRecordingFrontend {
    static constexpr frontend::Action action = frontend::Action::keepServerRunning;
    static inline ViewLaunch          received{};
    static inline int                 opens = 0;
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
    static void open(std::string_view, std::string_view, std::string_view, const ContentPolicy&, const WindowOptions&, const ViewLaunch& launch) {
        received = launch;
        ++opens;
    }
};
}  // namespace

const Scenario frontendAccepts{"A frontend that accepts a view launch receives this application's secret and origin", [] {
    given("a frontend with the richest open overload", [&] {
        using LaunchApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, LaunchRecordingFrontend>;
        LaunchApp first;
        LaunchApp second;
        when("each application opens its window", [&] {
            LaunchRecordingFrontend::opens = 0;
            first.openWindow("index.html");
            const auto firstLaunch = LaunchRecordingFrontend::received;
            second.openWindow("index.html");
            const auto secondLaunch = LaunchRecordingFrontend::received;
            then("the frontend gets a 64-character secret and the origin it must expose it to", [&] {
                require(LaunchRecordingFrontend::opens == 2);
                check(firstLaunch.secret.size() == 64);
                check(firstLaunch.origin == "http://127.0.0.1:" + std::string(first.port()));
            });
            then("every application has its own secret", [&] {
                check(firstLaunch.secret != secondLaunch.secret);
            });
        });
    });
}};

// Host integration (#567): a launch that returns, a close request from the host and an application on its own thread.
namespace {
struct LaunchRecorder {
    static inline std::string     url;
    static inline std::thread::id thread;
    static inline bool            fail = false;
    static constexpr frontend::Action action = frontend::Action::keepServerRunning;
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) {
        throw std::logic_error("start() must not call open()");
    }
    static void launch(std::string_view address, std::string_view port, std::string_view file) {
        thread = std::this_thread::get_id();
        url    = frontend::detail::applicationUrl(address, port, file);
        if (fail)
            throw std::runtime_error("no browser to launch");
    }
};

// A window that a host can ask to close. With lateWindow, open() creates it only after one request was refused.
template <Failure failure = Failure::none, bool lateWindow = false>
struct HostFrontend : ContractFrontend<frontend::Action::closeServerAfterOpen, failure> {
    static inline std::atomic<int> refused = 0;

    static void open(std::string_view port, std::string_view file) {
        if constexpr (lateWindow) {
            const auto deadline = std::chrono::steady_clock::now() + coordinationTimeout;
            while (refused == 0 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
        }
        ContractFrontend<frontend::Action::closeServerAfterOpen, failure>::open(port, file);
    }
    static bool requestClose() {
        const auto      state = ContractIoContext::current;
        std::lock_guard lock(state->mutex);
        if (!state->windowCreated) {
            ++refused;
            return false;
        }
        state->closeWindow = true;
        state->changed.notify_all();
        return true;
    }
};

// A window that accepts a close request only on its Nth attempt, as a slow or busy interface thread does; with
// throwing, the earlier attempts throw instead of returning false.
template <int attempts, bool throwing = false>
struct SlowCloseFrontend : ContractFrontend<frontend::Action::closeServerAfterOpen> {
    static inline std::atomic<int> requests = 0;
    static bool                    requestClose() {
        const auto      state = ContractIoContext::current;
        std::lock_guard lock(state->mutex);
        if (++requests < attempts) {
            if constexpr (throwing)
                throw std::runtime_error("transient close failure");
            return false;
        }
        state->closeWindow = true;
        state->changed.notify_all();
        return true;
    }
};

// Counts the close requests that reach the frontend, which is process-wide: with two applications, only the one that
// owns the window may cause one. `ownsExecutor` makes it publish its interface thread like the embedded frontends, and
// refuse a second open() while a session exists; `hold` keeps an open() before that point until the test lets it go.
template <bool ownsExecutor>
struct SharedWindowFrontend : ContractFrontend<frontend::Action::closeServerAfterOpen> {
    static inline std::atomic<int>  requests = 0;
    static inline std::atomic<bool> hold     = false;
    static inline std::atomic<int>  waiting  = 0;  // open() calls kept by `hold`, counted when they start waiting
    static inline std::function<void()> insideOpen;  // run on the interface thread once the session exists

    [[nodiscard]] static frontend::UIExecutor uiExecutor()
        requires ownsExecutor
    {
        return frontend::uiExecutorHostOf<SharedWindowFrontend>().executor();
    }
    static void open(std::string_view port, std::string_view file) {
        if (hold) {
            ++waiting;
            while (hold)
                std::this_thread::yield();
        }
        if constexpr (ownsExecutor) {
            auto& host = frontend::uiExecutorHostOf<SharedWindowFrontend>();
            host.attach([] { });  // throws std::logic_error when another session runs
            struct Detach {
                ~Detach() { frontend::uiExecutorHostOf<SharedWindowFrontend>().close(); }
            } detach;
            if (insideOpen)
                insideOpen();
            ContractFrontend<frontend::Action::closeServerAfterOpen>::open(port, file);
        } else {
            ContractFrontend<frontend::Action::closeServerAfterOpen>::open(port, file);
        }
    }
    static bool requestClose() {
        ++requests;
        return true;
    }
};

// Releases the held open() calls when the scope ends, also by a failed REQUIRE, so that no joined thread stays blocked.
// Declare a ReleaseHold after the threads that may wait on it: it is destroyed first, before their join.
template <typename Frontend>
struct HoldGuard {
    HoldGuard() { Frontend::hold = true; }
    ~HoldGuard() { Frontend::hold = false; }
    HoldGuard(const HoldGuard&) = delete;
    HoldGuard& operator=(const HoldGuard&) = delete;
};
template <typename Frontend>
struct ReleaseHold {
    ~ReleaseHold() { Frontend::hold = false; }
};

// A window that ignores every close request, until the test closes it.
struct DeafFrontend : ContractFrontend<frontend::Action::closeServerAfterOpen> {
    static bool requestClose() {
        return false;
    }
};

// A launching frontend that also takes the content policy.
struct PolicyLaunchRecorder : LaunchRecorder {
    static inline bool receivedPolicy = false;
    using LaunchRecorder::launch;
    static void launch(std::string_view address, std::string_view port, std::string_view file, const ContentPolicy&) {
        receivedPolicy = true;
        LaunchRecorder::launch(address, port, file);
    }
};

struct UninitializableFrontend : HostFrontend<> {
    static void initialize() {
        throw std::runtime_error("no display");
    }
};

template <typename Frontend>
using HostApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, Frontend>;

bool windowOpen(const std::shared_ptr<LoopState>& state) {
    std::unique_lock lock(state->mutex);
    return state->windowCreated && state->running;
}

static_assert(!frontend::requiresMainThread<frontend::DefaultBrowserFrontend>);
static_assert(!frontend::requiresMainThread<frontend::WebView2Frontend>);
static_assert(frontend::requiresMainThread<frontend::WKWebViewFrontend>);
static_assert(frontend::requiresMainThread<frontend::WebKitGTKFrontend>);
static_assert(frontend::requiresMainThread<frontend::CEFFrontend>);
static_assert(frontend::requiresMainThread<frontend::NativeOrBrowserFrontendFor<frontend::WKWebViewFrontend, frontend::DefaultBrowserFrontend>>);
static_assert(!frontend::requiresMainThread<LaunchRecorder>);  // undeclared: any thread
// A thread can be joined only when every session of the frontend ends on request.
static_assert(frontend::endsOnRequest<frontend::WebView2Frontend>);
static_assert(frontend::endsOnRequest<HostFrontend<>>);
static_assert(!frontend::endsOnRequest<frontend::DefaultBrowserFrontend>);
static_assert(!frontend::endsOnRequest<frontend::NativeOrBrowserFrontendFor<frontend::WebView2Frontend, frontend::DefaultBrowserFrontend>>);
static_assert(frontend::endsOnRequest<frontend::NativeOrBrowserFrontendFor<frontend::WebView2Frontend, frontend::BrowserWithUILoopFrontend>>);
}  // namespace

const Scenario startOpens{"start() opens the interface and returns to the host with the server running", [] {
    given("a frontend that can launch without a loop", [&] {
        LaunchRecorder::fail = false;
        LaunchRecorder::url.clear();
        std::shared_ptr<LoopState> state;
        {
            HostApp<LaunchRecorder> app;
            state = ContractIoContext::current;
            when("the host starts the application", [&] {
                app.start("page.html");
                then("the page was launched on the calling thread and the server loop runs on its own", [&] {
                    check(LaunchRecorder::thread == std::this_thread::get_id());
                    check(LaunchRecorder::url == "http://127.0.0.1:9002/page.html");
                    std::unique_lock lock(state->mutex);
                    check(state->changed.wait_for(lock, coordinationTimeout, [&] { return state->running; }));
                    check(!state->stopped);
                });
                andThen("the frontend has no window to close", [&] {
                    check(!app.requestClose());
                });
            });
            when("the launch fails", [&] {
                LaunchRecorder::fail = true;
                then("the server is stopped and joined before the error reaches the host", [&] {
                    requireThrows([&] { app.start(); }, equals("no browser to launch"));
                    std::lock_guard lock(state->mutex);
                    check(state->stopped);
                    check(state->exited);
                    check(!state->running);
                });
            });
        }
        // Destruction is the host's stop: the loop has been left, without waiting for its timeout.
        std::lock_guard lock(state->mutex);
        check(state->stopped);
        check(state->exited);
        check(!state->timedOut);
    });
}};

const Scenario requestCloseEnds{"requestClose() ends show() from another thread", [] {
    given("an embedded window shown on one thread", [&] {
        HostApp<HostFrontend<>> app;
        const auto              state = ContractIoContext::current;
        check(!app.requestClose());  // nothing is open yet
        std::jthread            caller([&] {
            app.show();
        });
        requireEventually([&] { return windowOpen(state); });
        when("the host asks for the closure from its own thread", [&] {
            check(app.requestClose());
            caller.join();
            then("show has returned after stopping the server", [&] {
                std::lock_guard lock(state->mutex);
                check(state->stopped);
                check(state->exited);
            });
        });
    });
}};

const Scenario uIThreadRuns{"UIThread runs an application on its own thread while the host keeps its thread", [] {
    given("a host that starts its interface", [&] {
        std::thread::id setupThread;
        std::string     port;
        UIThread<HostApp<HostFrontend<>>> ui(WebFrontConfig{}, [&](auto& app) {
            setupThread = std::this_thread::get_id();
            port        = std::string(app.port());
        });
        const auto state = ContractIoContext::current;
        then("the constructor returned with the application set up on the interface thread", [&] {
            check(setupThread != std::this_thread::get_id());
            check(setupThread != std::thread::id{});
            check(port == "9002");
            check(ui.app().port() == "9002");
        });
        requireEventually([&] { return windowOpen(state); });
        check(ui.running());
        check(!ui.waitFor(0ms));
        {
            std::lock_guard lock(state->mutex);
            check(state->openThread == setupThread);
        }
        when("the host stops it", [&] {
            requireNothrow([&] { ui.stop(); });
            then("the window closed, the server loop ended and nothing is left running", [&] {
                check(!ui.running());
                check(!ui.failure());
                std::lock_guard lock(state->mutex);
                check(state->closeWindow);
                check(state->stopped);
                check(state->exited);
                check(!state->timedOut);
            });
            andThen("stopping again does nothing", [&] {
                requireNothrow([&] { ui.stop(); });
            });
        });
        when("the user closes the window", [&] {
            {
                std::lock_guard lock(state->mutex);
                state->closeWindow = true;
                state->changed.notify_all();
            }
            then("the host observes the end and the application stays usable until stop", [&] {
                require(ui.waitFor(coordinationTimeout));
                check(!ui.running());
                check(ui.app().port() == "9002");
                requireNothrow([&] { ui.stop(); });
            });
        });
    });
    given("a window that is still opening when the host stops", [&] {
        using Late    = HostFrontend<Failure::none, true>;
        Late::refused = 0;
        UIThread<HostApp<Late>> ui(WebFrontConfig{}, [](auto&) { });
        const auto              state = ContractIoContext::current;
        when("stop is called at once", [&] {
            requireNothrow([&] { ui.stop(); });
            then("the request was repeated until the window existed, and the thread was joined", [&] {
                check(Late::refused >= 1);
                std::lock_guard lock(state->mutex);
                check(state->windowCreated);
                check(state->exited);
            });
        });
    });
}};

const Scenario uIThreadReports{"UIThread reports start and opening failures to the host", [] {
    given("a setup that throws", [&] {
        then("the constructor rethrows it on the host thread", [&] {
            using Hosted = UIThread<HostApp<HostFrontend<>>>;
            requireThrows([&] { Hosted(WebFrontConfig{}, [](auto&) { throw std::runtime_error("setup failed"); }); }, equals("setup failed"));
        });
    });
    given("a frontend that cannot be initialized on that thread", [&] {
        then("the constructor rethrows the initialization error", [&] {
            using Hosted = UIThread<HostApp<UninitializableFrontend>>;
            requireThrows([&] { Hosted(WebFrontConfig{}, [](auto&) { }); }, equals("no display"));
        });
    });
    given("a window that fails to open", [&] {
        UIThread<HostApp<HostFrontend<Failure::beforeWindow>>> ui(WebFrontConfig{}, [](auto&) { });
        const auto                                             state = ContractIoContext::current;
        when("the host waits for the session", [&] {
            require(ui.waitFor(coordinationTimeout));
            then("the failure is observable without throwing, then rethrown once by stop", [&] {
                check(!ui.running());
                require(ui.failure());
                checkThrows([&] { std::rethrow_exception(ui.failure()); }, equals("frontend failed before window creation"));
                requireThrows([&] { ui.stop(); }, equals("frontend failed before window creation"));
                requireNothrow([&] { ui.stop(); });
                std::lock_guard lock(state->mutex);
                check(state->stopped);
                check(state->exited);
            });
        });
    });
}};

const Scenario startCalled{"start() is called once and replaces show()", [] {
    given("an application already started", [&] {
        LaunchRecorder::fail = false;
        HostApp<LaunchRecorder> app;
        const auto              state = ContractIoContext::current;
        app.start();
        LaunchRecorder::url.clear();
        when("start is called again", [&] {
            then("it is refused before any launch and the server keeps running", [&] {
                requireThrows<std::logic_error>([&] { app.start(); });
                check(LaunchRecorder::url.empty());
                std::unique_lock lock(state->mutex);
                check(state->changed.wait_for(lock, coordinationTimeout, [&] { return state->running; }));
                check(!state->stopped);
            });
        });
        when("show is called", [&] {
            then("it is refused without opening or stopping anything", [&] {
                requireThrows<std::logic_error>([&] { app.show(); });
                std::lock_guard lock(state->mutex);
                check(!state->stopped);
            });
        });
    });
    given("a frontend that was shown and can also launch", [&] {
        struct Both : LaunchRecorder {
            static void open(std::string_view, std::string_view) { }
        };
        LaunchRecorder::url.clear();
        HostApp<Both> app;
        const auto    state = ContractIoContext::current;
        app.show();
        then("a later start is refused and leaves the server of show running", [&] {
            requireThrows<std::logic_error>([&] { app.start(); });
            check(LaunchRecorder::url.empty());
            std::lock_guard lock(state->mutex);
            check(!state->stopped);
        });
    });
    given("a window frontend that can also launch, shown and closed so that show joined its server", [&] {
        struct Window : HostFrontend<> {
            static void launch(std::string_view, std::string_view, std::string_view) {
                LaunchRecorder::url = "launched";
            }
        };
        LaunchRecorder::url.clear();
        HostApp<Window> app;
        const auto      state = ContractIoContext::current;
        {
            std::jthread host([&] {
                if (waitUntil([&] {
                        return windowOpen(state);
                    }))
                    while (!app.requestClose())
                        std::this_thread::yield();
            });
            app.show();
        }
        then("a later start is refused although no server thread is left to see (#567)", [&] {
            requireThrows<std::logic_error>([&] { app.start(); });
            check(LaunchRecorder::url.empty());
        });
    });
    given("a launch that failed", [&] {
        LaunchRecorder::fail = true;
        HostApp<LaunchRecorder> app;
        requireThrows([&] { app.start(); }, equals("no browser to launch"));
        LaunchRecorder::fail = false;
        then("the server is stopped for good and a new attempt is refused, not served by a dead server", [&] {
            requireThrows<std::logic_error>([&] { app.start(); });
        });
    });
    given("a frontend whose launch takes the content policy", [&] {
        PolicyLaunchRecorder::receivedPolicy = false;
        HostApp<PolicyLaunchRecorder> app;
        app.start();
        then("start gives it the application's policy", [&] {
            check(PolicyLaunchRecorder::receivedPolicy);
        });
    });
}};

const Scenario uIThreadWaits{"UIThread waits for a window that takes longer than one retry period to close", [] {
    given("a window that accepts the request on its fifth attempt (about forty milliseconds)", [&] {
        using Slow     = SlowCloseFrontend<5>;
        Slow::requests = 0;
        then("the destructor waits for it instead of reporting a stall and leaving its thread joinable (#567)", [&] {
            std::shared_ptr<LoopState> state;
            {
                UIThread<HostApp<Slow>> ui(WebFrontConfig{}, [](auto&) { });
                state = ContractIoContext::current;
                requireEventually([&] { return windowOpen(state); });
            }  // an overflowing patience made this throw inside the destructor and terminate the process
            std::lock_guard lock(state->mutex);
            check(state->closeWindow);
            check(state->stopped);  // show() returned and the application was destroyed after the window closed
            check(Slow::requests >= 5);
        });
    });
}};

const Scenario uIThreadJoins{"UIThread joins its thread when a close request throws", [] {
    given("a frontend whose first close requests throw", [&] {
        using Flaky     = SlowCloseFrontend<3, true>;
        Flaky::requests = 0;
        UIThread<HostApp<Flaky>> ui(WebFrontConfig{}, [](auto&) { });
        requireEventually([&] { return windowOpen(ContractIoContext::current); });
        when("the host stops the session", [&] {
            then("stop repeats the request until the window closes and joins", [&] {
                requireNothrow([&] { ui.stop(); });
                check(!ui.running());
                check(Flaky::requests >= 3);
            });
        });
    });
    given("a frontend whose close requests throw until the object is destroyed", [&] {
        using Flaky     = SlowCloseFrontend<4, true>;
        Flaky::requests = 0;
        then("the destructor still joins", [&] {
            requireNothrow([&] { [&] {
                UIThread<HostApp<Flaky>> ui(WebFrontConfig{}, [](auto&) { });
                requireEventually([&] { return windowOpen(ContractIoContext::current); });
            }(); });
        });
    });
}};

const Scenario applicationCloses{"An application closes only the window that it shows", [] {
    given("two applications of the same frontend, the first showing a window", [&] {
        using Shared     = SharedWindowFrontend<false>;
        Shared::requests = 0;
        Shared::hold     = false;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        requireEventually([&] { return windowOpen(firstState); });
        HostApp<Shared> second;  // shows nothing
        when("the second asks to close", [&] {
            then("nothing reaches the frontend: the window is the first application's (#567)", [&] {
                check(!second.requestClose());
                check(Shared::requests == 0);
                std::lock_guard lock(firstState->mutex);
                check(!firstState->closeWindow);
            });
        });
        when("the first asks to close", [&] {
            then("its own request is forwarded", [&] {
                check(first.requestClose());
                check(Shared::requests == 1);
            });
        });
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
    });
    given("a second application whose show() runs while the first owns the interface thread", [&] {
        using Shared     = SharedWindowFrontend<true>;
        Shared::requests = 0;
        Shared::hold     = false;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        requireEventually([&] { return windowOpen(firstState); });
        const HoldGuard<Shared> held;  // the second open() would wait before it is refused
        HostApp<Shared>    second;
        const auto         secondState = ContractIoContext::current;
        std::exception_ptr refusal;
        std::jthread       secondShow([&] {
            try {
                second.show();
            } catch (...) {
                refusal = std::current_exception();
            }
        });
        const ReleaseHold<Shared> release;
        when("the second asks to close while its open() is pending", [&] {
            then("the first application's window is not closed (#567)", [&] {
                check(!second.requestClose());
                check(Shared::requests == 0);
            });
        });
        Shared::hold = false;
        secondShow.join();
        then("its open() was refused, and it still cannot close the first's window", [&] {
            check(refusal != nullptr);
            check(!second.requestClose());
            check(Shared::requests == 0);
            std::lock_guard lock(firstState->mutex);
            check(!firstState->closeWindow);
        });
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
        (void)secondState;
    });
}};

const Scenario showCannot{"A show() that cannot own the session is refused before it opens a window", [] {
    given("a frontend without an executor, the first application showing", [&] {
        using Shared     = SharedWindowFrontend<false>;
        Shared::requests = 0;
        Shared::waiting  = 0;
        Shared::hold     = false;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        requireEventually([&] { return windowOpen(firstState); });
        HostApp<Shared> second;
        then("the second show() is refused and its close request reaches nothing (#567)", [&] {
            requireThrows<std::logic_error>([&] { second.show(); });
            check(!second.requestClose());
            check(Shared::requests == 0);
        });
        std::lock_guard lock(firstState->mutex);
        firstState->closeWindow = true;
        firstState->changed.notify_all();
    });
    given("a second show() called from the interface thread of the first", [&] {
        using Shared       = SharedWindowFrontend<true>;
        Shared::requests   = 0;
        Shared::waiting    = 0;
        Shared::hold       = false;
        HostApp<Shared>    first;
        const auto         firstState = ContractIoContext::current;
        HostApp<Shared>    second;  // the frontend reads the state of the latest application: made after `first` captured its own
        std::atomic<bool>  refused{false};
        std::atomic<bool>  hookDone{false};
        Shared::insideOpen = [&] {
            try {
                second.show();  // the same thread, inside the first's open()
            } catch (const std::logic_error&) {
                refused = true;
            }
            ContractIoContext::current = firstState;  // back to the first's state, which its open() is about to read
            hookDone                   = true;
        };
        std::jthread firstShow([&] { first.show(); });
        requireEventually([&] { return hookDone.load(); });
        Shared::insideOpen = nullptr;
        then("it is refused, cannot close the first's window, and the first can close its own (#567)", [&] {
            check(refused);
            check(!second.requestClose());
            check(Shared::requests == 0);
            requireEventually([&] { return windowOpen(firstState); });
            check(first.requestClose());
            check(Shared::requests == 1);
        });
        std::lock_guard lock(firstState->mutex);
        firstState->closeWindow = true;
        firstState->changed.notify_all();
    });
    given("an application refused while the first showed, and another that tries after the first ended", [&] {
        using Shared     = SharedWindowFrontend<true>;
        Shared::requests = 0;
        Shared::waiting  = 0;
        Shared::hold     = false;
        Shared::insideOpen = nullptr;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        requireEventually([&] { return windowOpen(firstState); });
        {
            HostApp<Shared> refused;  // a refused application is finished: its server was stopped with the refusal
            requireThrows<std::logic_error>([&] { refused.show(); });
        }
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
        firstShow.join();
        HostApp<Shared> second;  // the next attempt, as a UIThread makes it: a new application
        const auto      secondState = ContractIoContext::current;
        std::jthread    secondShow([&] { second.show(); });
        requireEventually([&] { return windowOpen(secondState); });
        then("it owns its window and the host can close it, instead of a window nobody can close (#567)", [&] {
            checkEventually([&] { return second.requestClose(); });
            check(Shared::requests >= 1);
        });
        std::lock_guard lock(secondState->mutex);
        secondState->closeWindow = true;
        secondState->changed.notify_all();
    });
    given("a second show() held before the frontend while the first one ends", [&] {
        using Shared       = SharedWindowFrontend<true>;
        Shared::requests   = 0;
        Shared::waiting    = 0;
        Shared::hold       = false;
        Shared::insideOpen = nullptr;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        requireEventually([&] { return windowOpen(firstState); });
        std::optional<HoldGuard<Shared>> held{std::in_place};
        HostApp<Shared>   second;
        const auto        secondState = ContractIoContext::current;
        std::atomic<bool> refused{false};
        std::jthread      secondShow([&] {
            try {
                second.show();
            } catch (const std::logic_error&) {
                refused = true;
            }
        });
        const ReleaseHold<Shared> release;
        // Refused at once, or (without the ownership rule) held in front of the frontend.
        requireEventually([&] { return refused.load() || Shared::waiting == 1; });
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
        firstShow.join();  // the first session is over before the second could attach
        held.reset();
        {
            std::lock_guard lock(secondState->mutex);  // releases a window that opens anyway, so that the test ends
            secondState->closeWindow = true;
            secondState->changed.notify_all();
        }
        secondShow.join();
        then("it never opens a window that the host could not close (#567)", [&] {
            check(refused);
        });
    });
}};

namespace {
// A frontend that offers only the close request that takes a session (no requestClose() without argument).
struct SessionOnlyFrontend : ContractFrontend<frontend::Action::closeServerAfterOpen> {
    static inline std::atomic<int> requests = 0;
    [[nodiscard]] static frontend::UIExecutor uiExecutor() {
        return frontend::uiExecutorHostOf<SessionOnlyFrontend>().executor();
    }
    static void open(std::string_view port, std::string_view file) {
        auto& host = frontend::uiExecutorHostOf<SessionOnlyFrontend>();
        host.attach([] { });
        struct Detach {
            ~Detach() { frontend::uiExecutorHostOf<SessionOnlyFrontend>().close(); }
        } detach;
        ContractFrontend<frontend::Action::closeServerAfterOpen>::open(port, file);
    }
    static bool requestClose(std::optional<frontend::UISession> session) {
        if (!session || session != uiExecutor().session())
            return false;
        ++requests;
        const auto      state = ContractIoContext::current;
        std::lock_guard lock(state->mutex);
        state->closeWindow = true;
        state->changed.notify_all();
        return true;
    }
};
static_assert(frontend::CanRequestClose<SessionOnlyFrontend>);
static_assert(frontend::endsOnRequest<SessionOnlyFrontend>);
}  // namespace

const Scenario secondShowSame{"A second show() on the same application leaves the first one's server and window alone", [] {
    given("an application showing a window", [&] {
        using Shared       = SharedWindowFrontend<true>;
        Shared::requests   = 0;
        Shared::hold       = false;
        Shared::insideOpen = nullptr;
        HostApp<Shared> app;
        const auto      state = ContractIoContext::current;
        when("show() is called again from another thread", [&] {
            std::jthread show([&] { app.show(); });
            requireEventually([&] { return windowOpen(state); });
            then("it is refused and the server keeps running, with its window still closable (#567)", [&] {
                requireThrows<std::logic_error>([&] { app.show(); });
                {
                    std::lock_guard lock(state->mutex);
                    check(!state->stopped);
                }
                checkEventually([&] { return app.requestClose(); });
            });
            std::lock_guard lock(state->mutex);
            state->closeWindow = true;
            state->changed.notify_all();
        });
        when("show() is called again from the interface thread, inside open()", [&] {
            std::atomic<bool> refused{false};
            std::atomic<bool> stoppedByRefusal{true};
            std::atomic<bool> hookDone{false};
            Shared::insideOpen = [&] {
                try {
                    app.show();
                } catch (const std::logic_error&) {
                    refused = true;
                }
                {
                    std::lock_guard lock(state->mutex);
                    stoppedByRefusal = state->stopped;
                }
                hookDone = true;
            };
            std::jthread show([&] { app.show(); });
            requireEventually([&] { return hookDone.load(); });
            Shared::insideOpen = nullptr;
            then("the nested call is refused without stopping the server of the first (#567)", [&] {
                check(refused);
                check(!stoppedByRefusal);
                requireEventually([&] { return windowOpen(state); });
                checkEventually([&] { return app.requestClose(); });
            });
            std::lock_guard lock(state->mutex);
            state->closeWindow = true;
            state->changed.notify_all();
        });
    });
}};

const Scenario frontendOffers{"A frontend that offers only the close request with a session can be closed", [] {
    given("an application shown through it", [&] {
        SessionOnlyFrontend::requests = 0;
        HostApp<SessionOnlyFrontend> app;
        const auto                   state = ContractIoContext::current;
        std::jthread                 show([&] { app.show(); });
        requireEventually([&] { return windowOpen(state); });
        then("requestClose() reaches it with the session of its show() (#567)", [&] {
            checkEventually([&] { return app.requestClose(); });
            check(SessionOnlyFrontend::requests == 1);
        });
        std::lock_guard lock(state->mutex);
        state->closeWindow = true;
        state->changed.notify_all();
    });
}};

const Scenario nativeOrBrowserFrontendForwards{"NativeOrBrowserFrontend forwards a close request when its native frontend has no executor", [] {
    given("a native frontend that offers only requestClose()", [&] {
        using Either = frontend::NativeOrBrowserFrontendFor<HostFrontend<>, DeafFrontend>;
        HostApp<Either> app;
        const auto      state = ContractIoContext::current;
        check(!Either::publishesUIExecutor());
        std::jthread show([&] { app.show(); });
        requireEventually([&] { return windowOpen(state); });
        then("the empty executor is not taken for a session that is not there (#567)", [&] {
            check(app.requestClose());
        });
    });
}};

const Scenario uIThreadStop{"UIThread::stop reports a window that does not close", [] {
    given("a window that ignores close requests", [&] {
        UIThread<HostApp<DeafFrontend>> ui(WebFrontConfig{}, [](auto&) { });
        const auto                      state = ContractIoContext::current;
        requireEventually([&] { return windowOpen(state); });
        when("the host stops it with a bounded patience", [&] {
            then("stop throws, and the session is still running and usable", [&] {
                requireThrows<UIThreadStalled>([&] { ui.stop(20ms); });
                check(ui.running());
                check(ui.app().port() == "9002");
            });
        });
        // The window closes at last (here: the user): stop, and the destructor after it, then return.
        {
            std::lock_guard lock(state->mutex);
            state->closeWindow = true;
            state->changed.notify_all();
        }
        requireNothrow([&] { ui.stop(); });
    });
}};

const Scenario closeRequest{"A close request made for one session never closes the window of a later one", [] {
    given("a request prepared for the first loop of a frontend", [&] {
        frontend::UIExecutorHost   host;
        frontend::NativeWindowHost windowHost{host.executor()};
        int                        firstWindow  = 0;
        int                        secondWindow = 0;
        int                        closed       = 0;
        const auto                 close        = [&](const frontend::NativeWindowHandle& window) {
            closed = *static_cast<int*>(window.pointer) + 1;
        };
        host.attach([] { });
        const auto first = host.executor().session();
        require(first);
        windowHost.publish({.kind = frontend::NativeWindowKind::gtk, .pointer = &firstWindow, .portalParent = {}});
        // The first session ends, as its window closes; another application then opens its own on the same thread, with a
        // window of its own. Everything is sequenced here: the request is made after the check of the first session passed.
        windowHost.retract();
        host.close();
        host.attach([] { });
        const auto second = host.executor().session();
        require(second);
        windowHost.publish({.kind = frontend::NativeWindowKind::gtk, .pointer = &secondWindow, .portalParent = {}});
        check(first->thread == second->thread);  // the same thread does not make it the same session
        check(*first != *second);
        when("the request names the first session", [&] {
            require(frontend::detail::requestWindowClose(host.executor(), windowHost.window(), close, first));
            host.drain();
            then("the second session's window stays open", [&] {
                check(closed == 0);
            });
        });
        when("the request names the session that runs", [&] {
            require(frontend::detail::requestWindowClose(host.executor(), windowHost.window(), close, second));
            host.drain();
            then("its window is closed", [&] {
                check(closed == 1);
            });
        });
        host.close();
    });
}};

const Scenario systemBrowserLoop{"The system browser's loop ignores a close request made for an earlier loop", [] {
    given("a session of the first loop, kept after it ended", [&] {
        using Browser = frontend::BrowserWithUILoopFrontend;
        // The loop's stop state is process-wide and a test before this one may have left a request pending: spend it.
        Browser::requestStop();
        Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
        std::optional<frontend::UISession> stale;
        {
            std::jthread client([&] {
                const auto executor = Browser::uiExecutor();
                (void)waitUntil([&] {
                    return executor.available();
                });
                stale = executor.session();
                Browser::requestStop();
            });
            Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
        }
        require(stale);
        then("a later loop is not ended by it, and its own session ends it", [&] {
            bool                staleAccepted = true;
            bool                ownAccepted   = false;
            std::jthread        client([&] {
                const auto executor = Browser::uiExecutor();
                (void)waitUntil([&] {
                    return executor.available();
                });
                const auto own = executor.session();
                staleAccepted  = Browser::requestClose(stale);
                ownAccepted    = waitUntil([&] {
                    return Browser::requestClose(own);
                });
                if (!ownAccepted)
                    Browser::requestStop();
            });
            Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
            client.join();
            check(!staleAccepted);
            check(ownAccepted);
        });
    });
}};

#if !defined(_WIN32)
const Scenario systemBrowserLoopUp{"The system browser's loop is up while its opener is still running", [] {
    given("an opener that lives longer than the close request", [&] {
        namespace fs = std::filesystem;
        if (browser::isWSL())
            succeed("WSL opens the page through PowerShell, not through a command this test can replace");
        else {
            const auto directory = fs::temp_directory_path() / ("webfront-opener-" + std::to_string(::getpid()));
            fs::create_directories(directory);
            for (const char* name : {"xdg-open", "open"}) {
                std::ofstream script(directory / name);
                script << "#!/bin/sh\nsleep 3\n";
                fs::permissions(directory / name, fs::perms::owner_all);
            }
            const std::string path = directory.string() + ":" + (std::getenv("PATH") ? std::getenv("PATH") : "");
            const std::string saved = std::getenv("PATH") ? std::getenv("PATH") : "";
            ::setenv("PATH", path.c_str(), 1);
            using Browser = frontend::BrowserWithUILoopFrontend;
            Browser::requestStop();  // spend a request that an earlier test may have left pending
            Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
            std::atomic<bool> closed{false};
            std::jthread      host([&] {
                // The host's own thread: the loop must be there to receive the request well before the opener ends.
                closed = waitUntil([] {
                    return Browser::requestClose(Browser::uiExecutor().session());
                });
                if (!closed)
                    Browser::requestStop();
            });
            const auto start = std::chrono::steady_clock::now();
            Browser::open("9002", "index.html");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            host.join();
            ::setenv("PATH", saved.c_str(), 1);
            fs::remove_all(directory);
            then("the loop ran and ended at the request, not after the opener (#567)", [&] {
                check(closed);
                check(elapsed < 2500ms);
            });
        }
    });
}};

const Scenario systemBrowserOpener{"The system browser opener is started without being waited for", [] {
    given("a command that lives for two seconds", [&] {
        const auto start = std::chrono::steady_clock::now();
        std::mutex              mutex;
        std::condition_variable ended;
        std::optional<int>      status;
        browser::detail::spawnWithoutWaiting("sleep 2", [&](int value) {
            std::lock_guard lock(mutex);
            status = value;
            ended.notify_all();
        });
        then("the call returns long before it ends, and its end is reported afterwards (#567)", [&] {
            check(std::chrono::steady_clock::now() - start < 1s);
            std::unique_lock lock(mutex);
            check(!status.has_value());
            require(ended.wait_for(lock, 10s, [&] { return status.has_value(); }));
            check(*status == 0);
        });
    });
    given("a command that fails", [&] {
        std::mutex              mutex;
        std::condition_variable ended;
        std::optional<int>      status;
        browser::detail::spawnWithoutWaiting("exit 3", [&](int value) {
            std::lock_guard lock(mutex);
            status = value;
            ended.notify_all();
        });
        then("the status is reported, not thrown", [&] {
            std::unique_lock lock(mutex);
            require(ended.wait_for(lock, 10s, [&] { return status.has_value(); }));
            check(WIFEXITED(*status));
            check(WEXITSTATUS(*status) == 3);
        });
    });
}};
#endif

const Scenario windowCloseScenario{"A window close request is always released by the executor", [] {
    given("a close function that holds a resource", [&] {
        frontend::UIExecutorHost   host;
        frontend::NativeWindowHost windowHost{host.executor()};
        const auto                 resource = std::make_shared<int>(0);
        const auto                 request  = [&] {
            return frontend::detail::requestWindowClose(host.executor(), windowHost.window(), [resource](const frontend::NativeWindowHandle&) {
                ++*resource;
            });
        };
        when("no loop is attached", [&] {
            then("the request is refused and nothing is kept", [&] {
                check(!request());
                check(resource.use_count() == 1);
            });
        });
        when("the loop closes with the request pending", [&] {
            host.attach([] { });
            require(request());
            check(resource.use_count() == 2);
            host.close();
            then("it completes stopped: the window is not closed and the request is freed", [&] {
                check(*resource == 0);
                check(resource.use_count() == 1);
            });
        });
        when("the loop runs the request", [&] {
            host.attach([] { });
            andWhen("the window is published", [&] {
                int window = 0;
                windowHost.publish({.kind = frontend::NativeWindowKind::gtk, .pointer = &window, .portalParent = {}});
                require(request());
                host.drain();
                then("the close function ran once and the request is freed", [&] {
                    check(*resource == 1);
                    check(resource.use_count() == 1);
                });
            });
            andWhen("no window is published", [&] {
                require(request());
                host.drain();
                then("nothing is closed and the request is freed", [&] {
                    check(*resource == 0);
                    check(resource.use_count() == 1);
                });
            });
            host.close();
        });
    });
}};
