#include "Mocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
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
        CHECK(opened);
        CHECK(state->openThread == caller.get_id());
        CHECK(state->windowCreated);
        CHECK_FALSE(state->showReturned);
        CHECK_FALSE(state->stopped);
        // Release the simulated window before any fatal assertion or thread join.
        state->closeWindow = true;
        state->changed.notify_all();
    }
    caller.join();
    {
        std::lock_guard lock(state->mutex);
        if constexpr (onReturn == frontend::Action::closeServerAfterOpen || failure != Failure::none) {
            CHECK(state->stopped);
            CHECK(state->exited);
            CHECK_FALSE(state->running);
        } else {
            CHECK_FALSE(state->stopped);
            CHECK_FALSE(state->exited);
            CHECK(state->running);
        }
        CHECK_FALSE(state->timedOut);
    }
    // Keep cleanup safe even if a CHECK above failed.
    app.stop();
    if constexpr (failure == Failure::none) {
        CHECK_FALSE(error);
    } else {
        REQUIRE(error);
        if constexpr (failure == Failure::standard)
            CHECK_THROWS_WITH(std::rethrow_exception(error), "frontend startup failed");
        else
            CHECK_THROWS_AS(std::rethrow_exception(error), int);
    }
}

template <frontend::Action onReturn>
void checkStartupFailure() {
    using ContractApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, ContractFrontend<onReturn, Failure::beforeWindow>>;
    ContractApp        app;
    const auto state = ContractIoContext::current;
    REQUIRE_THROWS_WITH(app.show(), "frontend failed before window creation");
    std::lock_guard lock(state->mutex);
    CHECK(state->frontendEntered);
    CHECK_FALSE(state->windowCreated);
    CHECK_FALSE(state->closeWindow);
    CHECK(state->stopped);
    CHECK(state->exited);
    CHECK_FALSE(state->running);
    CHECK_FALSE(state->timedOut);
}
}  // namespace

SCENARIO("An embedded frontend blocks show until closure and completes the server loop") {
    GIVEN("a window held open while its server loop runs") {
        WHEN("show opens the window and the window is closed") {
            THEN("show runs open on its calling thread and waits for shutdown after closure") {
                checkSession<frontend::Action::closeServerAfterOpen>();
            }
        }
    }
}

SCENARIO("A launching frontend leaves the server loop running after show returns") {
    GIVEN("a frontend with keepServerRunning") {
        WHEN("show opens the frontend and open returns") {
            THEN("the server remains active until explicitly stopped") {
                checkSession<frontend::Action::keepServerRunning>();
            }
        }
    }
}

SCENARIO("Frontend exceptions propagate only after the server loop completes") {
    GIVEN("an embedded frontend that throws") {
        WHEN("open fails during window cleanup") {
            THEN("the original diagnostic survives shutdown") {
                checkSession<frontend::Action::closeServerAfterOpen, Failure::standard>();
            }
        }
    }
    GIVEN("a launching frontend that throws") {
        WHEN("open fails before returning") {
            THEN("its action does not prevent shutdown") {
                checkSession<frontend::Action::keepServerRunning, Failure::standard>();
            }
        }
    }
    GIVEN("a frontend throwing a non-standard exception") {
        WHEN("open throws during window cleanup") {
            THEN("shutdown also precedes propagation of that exception") {
                checkSession<frontend::Action::closeServerAfterOpen, Failure::nonStandard>();
            }
        }
    }
}

SCENARIO("A frontend startup failure before window creation completes the server loop") {
    GIVEN("a frontend that fails before creating a window") {
        WHEN("show selects an embedded frontend") {
            THEN("the server stops before the startup error propagates") {
                checkStartupFailure<frontend::Action::closeServerAfterOpen>();
            }
        }
        WHEN("show selects a launching frontend") {
            THEN("the same shutdown guarantee holds") {
                checkStartupFailure<frontend::Action::keepServerRunning>();
            }
        }
    }
}

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

SCENARIO("The application chooses the title and the size of the embedded window") {
    GIVEN("a configuration with a window title and size") {
        const auto window = windowReceivedBy({.window = {.title = "SiliconForge", .width = 640, .height = 480}});
        THEN("the frontend receives them unchanged") {
            CHECK(window.title == "SiliconForge");
            CHECK(window.width == 640);
            CHECK(window.height == 480);
        }
    }
    GIVEN("a default configuration") {
        const auto window = windowReceivedBy({});
        THEN("the frontend receives an empty title and the default size") {
            CHECK(window.title.empty());
            CHECK(window.width == 1200);
            CHECK(window.height == 800);
        }
        THEN("the effective title is the executable's name, never the former constant") {
            const auto effective = frontend::detail::resolveWindow(window);
            CHECK_FALSE(effective.title.empty());
            CHECK(effective.title != "WebFront Application");
        }
    }
    GIVEN("an invalid size and an explicit title") {
        const auto effective = frontend::detail::resolveWindow({.title = "T", .width = 0, .height = -5});
        THEN("the title is kept and the size falls back to the default") {
            CHECK(effective.title == "T");
            CHECK(effective.width == 1200);
            CHECK(effective.height == 800);
        }
    }
}

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

SCENARIO("A frontend that accepts a view launch receives this application's secret and origin") {
    GIVEN("a frontend with the richest open overload") {
        using LaunchApp = BasicWFWithFrontend<ContractNet, MockFileSystem<>, LaunchRecordingFrontend>;
        LaunchApp first;
        LaunchApp second;
        WHEN("each application opens its window") {
            LaunchRecordingFrontend::opens = 0;
            first.openWindow("index.html");
            const auto firstLaunch = LaunchRecordingFrontend::received;
            second.openWindow("index.html");
            const auto secondLaunch = LaunchRecordingFrontend::received;
            THEN("the frontend gets a 64-character secret and the origin it must expose it to") {
                REQUIRE(LaunchRecordingFrontend::opens == 2);
                CHECK(firstLaunch.secret.size() == 64);
                CHECK(firstLaunch.origin == "http://127.0.0.1:" + std::string(first.port()));
            }
            THEN("every application has its own secret") {
                CHECK(firstLaunch.secret != secondLaunch.secret);
            }
        }
    }
}

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

template <typename Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + coordinationTimeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

bool waitForWindow(const std::shared_ptr<LoopState>& state) {
    std::unique_lock lock(state->mutex);
    return state->changed.wait_for(lock, coordinationTimeout, [&] {
        return state->windowCreated && state->running;
    });
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

SCENARIO("start() opens the interface and returns to the host with the server running") {
    GIVEN("a frontend that can launch without a loop") {
        LaunchRecorder::fail = false;
        LaunchRecorder::url.clear();
        std::shared_ptr<LoopState> state;
        {
            HostApp<LaunchRecorder> app;
            state = ContractIoContext::current;
            WHEN("the host starts the application") {
                app.start("page.html");
                THEN("the page was launched on the calling thread and the server loop runs on its own") {
                    CHECK(LaunchRecorder::thread == std::this_thread::get_id());
                    CHECK(LaunchRecorder::url == "http://127.0.0.1:9002/page.html");
                    std::unique_lock lock(state->mutex);
                    CHECK(state->changed.wait_for(lock, coordinationTimeout, [&] { return state->running; }));
                    CHECK_FALSE(state->stopped);
                }
                AND_THEN("the frontend has no window to close") {
                    CHECK_FALSE(app.requestClose());
                }
            }
            WHEN("the launch fails") {
                LaunchRecorder::fail = true;
                THEN("the server is stopped and joined before the error reaches the host") {
                    REQUIRE_THROWS_WITH(app.start(), "no browser to launch");
                    std::lock_guard lock(state->mutex);
                    CHECK(state->stopped);
                    CHECK(state->exited);
                    CHECK_FALSE(state->running);
                }
            }
        }
        // Destruction is the host's stop: the loop has been left, without waiting for its timeout.
        std::lock_guard lock(state->mutex);
        CHECK(state->stopped);
        CHECK(state->exited);
        CHECK_FALSE(state->timedOut);
    }
}

SCENARIO("requestClose() ends show() from another thread") {
    GIVEN("an embedded window shown on one thread") {
        HostApp<HostFrontend<>> app;
        const auto              state = ContractIoContext::current;
        CHECK_FALSE(app.requestClose());  // nothing is open yet
        std::jthread            caller([&] {
            app.show();
        });
        REQUIRE(waitForWindow(state));
        WHEN("the host asks for the closure from its own thread") {
            CHECK(app.requestClose());
            caller.join();
            THEN("show has returned after stopping the server") {
                std::lock_guard lock(state->mutex);
                CHECK(state->stopped);
                CHECK(state->exited);
            }
        }
    }
}

SCENARIO("UIThread runs an application on its own thread while the host keeps its thread") {
    GIVEN("a host that starts its interface") {
        std::thread::id setupThread;
        std::string     port;
        UIThread<HostApp<HostFrontend<>>> ui(WebFrontConfig{}, [&](auto& app) {
            setupThread = std::this_thread::get_id();
            port        = std::string(app.port());
        });
        const auto state = ContractIoContext::current;
        THEN("the constructor returned with the application set up on the interface thread") {
            CHECK(setupThread != std::this_thread::get_id());
            CHECK(setupThread != std::thread::id{});
            CHECK(port == "9002");
            CHECK(ui.app().port() == "9002");
        }
        REQUIRE(waitForWindow(state));
        CHECK(ui.running());
        CHECK_FALSE(ui.waitFor(0ms));
        {
            std::lock_guard lock(state->mutex);
            CHECK(state->openThread == setupThread);
        }
        WHEN("the host stops it") {
            REQUIRE_NOTHROW(ui.stop());
            THEN("the window closed, the server loop ended and nothing is left running") {
                CHECK_FALSE(ui.running());
                CHECK_FALSE(ui.failure());
                std::lock_guard lock(state->mutex);
                CHECK(state->closeWindow);
                CHECK(state->stopped);
                CHECK(state->exited);
                CHECK_FALSE(state->timedOut);
            }
            AND_THEN("stopping again does nothing") {
                REQUIRE_NOTHROW(ui.stop());
            }
        }
        WHEN("the user closes the window") {
            {
                std::lock_guard lock(state->mutex);
                state->closeWindow = true;
                state->changed.notify_all();
            }
            THEN("the host observes the end and the application stays usable until stop") {
                REQUIRE(ui.waitFor(coordinationTimeout));
                CHECK_FALSE(ui.running());
                CHECK(ui.app().port() == "9002");
                REQUIRE_NOTHROW(ui.stop());
            }
        }
    }
    GIVEN("a window that is still opening when the host stops") {
        using Late    = HostFrontend<Failure::none, true>;
        Late::refused = 0;
        UIThread<HostApp<Late>> ui(WebFrontConfig{}, [](auto&) { });
        const auto              state = ContractIoContext::current;
        WHEN("stop is called at once") {
            REQUIRE_NOTHROW(ui.stop());
            THEN("the request was repeated until the window existed, and the thread was joined") {
                CHECK(Late::refused >= 1);
                std::lock_guard lock(state->mutex);
                CHECK(state->windowCreated);
                CHECK(state->exited);
            }
        }
    }
}

SCENARIO("UIThread reports start and opening failures to the host") {
    GIVEN("a setup that throws") {
        THEN("the constructor rethrows it on the host thread") {
            using Hosted = UIThread<HostApp<HostFrontend<>>>;
            REQUIRE_THROWS_WITH(Hosted(WebFrontConfig{}, [](auto&) { throw std::runtime_error("setup failed"); }), "setup failed");
        }
    }
    GIVEN("a frontend that cannot be initialized on that thread") {
        THEN("the constructor rethrows the initialization error") {
            using Hosted = UIThread<HostApp<UninitializableFrontend>>;
            REQUIRE_THROWS_WITH(Hosted(WebFrontConfig{}, [](auto&) { }), "no display");
        }
    }
    GIVEN("a window that fails to open") {
        UIThread<HostApp<HostFrontend<Failure::beforeWindow>>> ui(WebFrontConfig{}, [](auto&) { });
        const auto                                             state = ContractIoContext::current;
        WHEN("the host waits for the session") {
            REQUIRE(ui.waitFor(coordinationTimeout));
            THEN("the failure is observable without throwing, then rethrown once by stop") {
                CHECK_FALSE(ui.running());
                REQUIRE(ui.failure());
                CHECK_THROWS_WITH(std::rethrow_exception(ui.failure()), "frontend failed before window creation");
                REQUIRE_THROWS_WITH(ui.stop(), "frontend failed before window creation");
                REQUIRE_NOTHROW(ui.stop());
                std::lock_guard lock(state->mutex);
                CHECK(state->stopped);
                CHECK(state->exited);
            }
        }
    }
}

SCENARIO("start() is called once and replaces show()") {
    GIVEN("an application already started") {
        LaunchRecorder::fail = false;
        HostApp<LaunchRecorder> app;
        const auto              state = ContractIoContext::current;
        app.start();
        LaunchRecorder::url.clear();
        WHEN("start is called again") {
            THEN("it is refused before any launch and the server keeps running") {
                REQUIRE_THROWS_AS(app.start(), std::logic_error);
                CHECK(LaunchRecorder::url.empty());
                std::unique_lock lock(state->mutex);
                CHECK(state->changed.wait_for(lock, coordinationTimeout, [&] { return state->running; }));
                CHECK_FALSE(state->stopped);
            }
        }
        WHEN("show is called") {
            THEN("it is refused without opening or stopping anything") {
                REQUIRE_THROWS_AS(app.show(), std::logic_error);
                std::lock_guard lock(state->mutex);
                CHECK_FALSE(state->stopped);
            }
        }
    }
    GIVEN("a frontend that was shown and can also launch") {
        struct Both : LaunchRecorder {
            static void open(std::string_view, std::string_view) { }
        };
        LaunchRecorder::url.clear();
        HostApp<Both> app;
        const auto    state = ContractIoContext::current;
        app.show();
        THEN("a later start is refused and leaves the server of show running") {
            REQUIRE_THROWS_AS(app.start(), std::logic_error);
            CHECK(LaunchRecorder::url.empty());
            std::lock_guard lock(state->mutex);
            CHECK_FALSE(state->stopped);
        }
    }
    GIVEN("a window frontend that can also launch, shown and closed so that show joined its server") {
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
                if (waitForWindow(state))
                    while (!app.requestClose())
                        std::this_thread::yield();
            });
            app.show();
        }
        THEN("a later start is refused although no server thread is left to see (#567)") {
            REQUIRE_THROWS_AS(app.start(), std::logic_error);
            CHECK(LaunchRecorder::url.empty());
        }
    }
    GIVEN("a launch that failed") {
        LaunchRecorder::fail = true;
        HostApp<LaunchRecorder> app;
        REQUIRE_THROWS_WITH(app.start(), "no browser to launch");
        LaunchRecorder::fail = false;
        THEN("the server is stopped for good and a new attempt is refused, not served by a dead server") {
            REQUIRE_THROWS_AS(app.start(), std::logic_error);
        }
    }
    GIVEN("a frontend whose launch takes the content policy") {
        PolicyLaunchRecorder::receivedPolicy = false;
        HostApp<PolicyLaunchRecorder> app;
        app.start();
        THEN("start gives it the application's policy") {
            CHECK(PolicyLaunchRecorder::receivedPolicy);
        }
    }
}

SCENARIO("UIThread waits for a window that takes longer than one retry period to close") {
    GIVEN("a window that accepts the request on its fifth attempt (about forty milliseconds)") {
        using Slow     = SlowCloseFrontend<5>;
        Slow::requests = 0;
        THEN("the destructor waits for it instead of reporting a stall and leaving its thread joinable (#567)") {
            std::shared_ptr<LoopState> state;
            {
                UIThread<HostApp<Slow>> ui(WebFrontConfig{}, [](auto&) { });
                state = ContractIoContext::current;
                REQUIRE(waitForWindow(state));
            }  // an overflowing patience made this throw inside the destructor and terminate the process
            std::lock_guard lock(state->mutex);
            CHECK(state->closeWindow);
            CHECK(state->stopped);  // show() returned and the application was destroyed after the window closed
            CHECK(Slow::requests >= 5);
        }
    }
}

SCENARIO("UIThread joins its thread when a close request throws") {
    GIVEN("a frontend whose first close requests throw") {
        using Flaky     = SlowCloseFrontend<3, true>;
        Flaky::requests = 0;
        UIThread<HostApp<Flaky>> ui(WebFrontConfig{}, [](auto&) { });
        REQUIRE(waitForWindow(ContractIoContext::current));
        WHEN("the host stops the session") {
            THEN("stop repeats the request until the window closes and joins") {
                REQUIRE_NOTHROW(ui.stop());
                CHECK_FALSE(ui.running());
                CHECK(Flaky::requests >= 3);
            }
        }
    }
    GIVEN("a frontend whose close requests throw until the object is destroyed") {
        using Flaky     = SlowCloseFrontend<4, true>;
        Flaky::requests = 0;
        THEN("the destructor still joins") {
            REQUIRE_NOTHROW([&] {
                UIThread<HostApp<Flaky>> ui(WebFrontConfig{}, [](auto&) { });
                REQUIRE(waitForWindow(ContractIoContext::current));
            }());
        }
    }
}

SCENARIO("An application closes only the window that it shows") {
    GIVEN("two applications of the same frontend, the first showing a window") {
        using Shared     = SharedWindowFrontend<false>;
        Shared::requests = 0;
        Shared::hold     = false;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        REQUIRE(waitForWindow(firstState));
        HostApp<Shared> second;  // shows nothing
        WHEN("the second asks to close") {
            THEN("nothing reaches the frontend: the window is the first application's (#567)") {
                CHECK_FALSE(second.requestClose());
                CHECK(Shared::requests == 0);
                std::lock_guard lock(firstState->mutex);
                CHECK_FALSE(firstState->closeWindow);
            }
        }
        WHEN("the first asks to close") {
            THEN("its own request is forwarded") {
                CHECK(first.requestClose());
                CHECK(Shared::requests == 1);
            }
        }
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
    }
    GIVEN("a second application whose show() runs while the first owns the interface thread") {
        using Shared     = SharedWindowFrontend<true>;
        Shared::requests = 0;
        Shared::hold     = false;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        REQUIRE(waitForWindow(firstState));
        Shared::hold = true;  // the second open() waits before it is refused
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
        WHEN("the second asks to close while its open() is pending") {
            THEN("the first application's window is not closed (#567)") {
                CHECK_FALSE(second.requestClose());
                CHECK(Shared::requests == 0);
            }
        }
        Shared::hold = false;
        secondShow.join();
        THEN("its open() was refused, and it still cannot close the first's window") {
            CHECK(refusal != nullptr);
            CHECK_FALSE(second.requestClose());
            CHECK(Shared::requests == 0);
            std::lock_guard lock(firstState->mutex);
            CHECK_FALSE(firstState->closeWindow);
        }
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
        (void)secondState;
    }
}

SCENARIO("A show() that cannot own the session is refused before it opens a window") {
    GIVEN("a frontend without an executor, the first application showing") {
        using Shared     = SharedWindowFrontend<false>;
        Shared::requests = 0;
        Shared::waiting  = 0;
        Shared::hold     = false;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        REQUIRE(waitForWindow(firstState));
        HostApp<Shared> second;
        THEN("the second show() is refused and its close request reaches nothing (#567)") {
            REQUIRE_THROWS_AS(second.show(), std::logic_error);
            CHECK_FALSE(second.requestClose());
            CHECK(Shared::requests == 0);
        }
        std::lock_guard lock(firstState->mutex);
        firstState->closeWindow = true;
        firstState->changed.notify_all();
    }
    GIVEN("a second show() called from the interface thread of the first") {
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
        REQUIRE(eventually([&] { return hookDone.load(); }));
        Shared::insideOpen = nullptr;
        THEN("it is refused, cannot close the first's window, and the first can close its own (#567)") {
            CHECK(refused);
            CHECK_FALSE(second.requestClose());
            CHECK(Shared::requests == 0);
            REQUIRE(waitForWindow(firstState));
            CHECK(first.requestClose());
            CHECK(Shared::requests == 1);
        }
        std::lock_guard lock(firstState->mutex);
        firstState->closeWindow = true;
        firstState->changed.notify_all();
    }
    GIVEN("an application refused while the first showed, and another that tries after the first ended") {
        using Shared     = SharedWindowFrontend<true>;
        Shared::requests = 0;
        Shared::waiting  = 0;
        Shared::hold     = false;
        Shared::insideOpen = nullptr;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        REQUIRE(waitForWindow(firstState));
        {
            HostApp<Shared> refused;  // a refused application is finished: its server was stopped with the refusal
            REQUIRE_THROWS_AS(refused.show(), std::logic_error);
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
        REQUIRE(waitForWindow(secondState));
        THEN("it owns its window and the host can close it, instead of a window nobody can close (#567)") {
            CHECK(eventually([&] { return second.requestClose(); }));
            CHECK(Shared::requests >= 1);
        }
        std::lock_guard lock(secondState->mutex);
        secondState->closeWindow = true;
        secondState->changed.notify_all();
    }
    GIVEN("a second show() held before the frontend while the first one ends") {
        using Shared       = SharedWindowFrontend<true>;
        Shared::requests   = 0;
        Shared::waiting    = 0;
        Shared::hold       = false;
        Shared::insideOpen = nullptr;
        HostApp<Shared> first;
        const auto      firstState = ContractIoContext::current;
        std::jthread    firstShow([&] { first.show(); });
        REQUIRE(waitForWindow(firstState));
        Shared::hold = true;
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
        // Refused at once, or (without the ownership rule) held in front of the frontend.
        REQUIRE(eventually([&] { return refused.load() || Shared::waiting == 1; }));
        {
            std::lock_guard lock(firstState->mutex);
            firstState->closeWindow = true;
            firstState->changed.notify_all();
        }
        firstShow.join();  // the first session is over before the second could attach
        Shared::hold = false;
        {
            std::lock_guard lock(secondState->mutex);  // releases a window that opens anyway, so that the test ends
            secondState->closeWindow = true;
            secondState->changed.notify_all();
        }
        secondShow.join();
        THEN("it never opens a window that the host could not close (#567)") {
            CHECK(refused);
        }
    }
}

SCENARIO("NativeOrBrowserFrontend forwards a close request when its native frontend has no executor") {
    GIVEN("a native frontend that offers only requestClose()") {
        using Either = frontend::NativeOrBrowserFrontendFor<HostFrontend<>, DeafFrontend>;
        HostApp<Either> app;
        const auto      state = ContractIoContext::current;
        CHECK_FALSE(Either::publishesUIExecutor());
        std::jthread show([&] { app.show(); });
        REQUIRE(waitForWindow(state));
        THEN("the empty executor is not taken for a session that is not there (#567)") {
            CHECK(app.requestClose());
        }
    }
}

SCENARIO("UIThread::stop reports a window that does not close") {
    GIVEN("a window that ignores close requests") {
        UIThread<HostApp<DeafFrontend>> ui(WebFrontConfig{}, [](auto&) { });
        const auto                      state = ContractIoContext::current;
        REQUIRE(waitForWindow(state));
        WHEN("the host stops it with a bounded patience") {
            THEN("stop throws, and the session is still running and usable") {
                REQUIRE_THROWS_AS(ui.stop(20ms), UIThreadStalled);
                CHECK(ui.running());
                CHECK(ui.app().port() == "9002");
            }
        }
        // The window closes at last (here: the user): stop, and the destructor after it, then return.
        {
            std::lock_guard lock(state->mutex);
            state->closeWindow = true;
            state->changed.notify_all();
        }
        REQUIRE_NOTHROW(ui.stop());
    }
}

SCENARIO("A close request made for one session never closes the window of a later one") {
    GIVEN("a request prepared for the first loop of a frontend") {
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
        REQUIRE(first);
        windowHost.publish({.kind = frontend::NativeWindowKind::gtk, .pointer = &firstWindow, .portalParent = {}});
        // The first session ends, as its window closes; another application then opens its own on the same thread, with a
        // window of its own. Everything is sequenced here: the request is made after the check of the first session passed.
        windowHost.retract();
        host.close();
        host.attach([] { });
        const auto second = host.executor().session();
        REQUIRE(second);
        windowHost.publish({.kind = frontend::NativeWindowKind::gtk, .pointer = &secondWindow, .portalParent = {}});
        CHECK(first->thread == second->thread);  // the same thread does not make it the same session
        CHECK(*first != *second);
        WHEN("the request names the first session") {
            REQUIRE(frontend::detail::requestWindowClose(host.executor(), windowHost.window(), close, first));
            host.drain();
            THEN("the second session's window stays open") {
                CHECK(closed == 0);
            }
        }
        WHEN("the request names the session that runs") {
            REQUIRE(frontend::detail::requestWindowClose(host.executor(), windowHost.window(), close, second));
            host.drain();
            THEN("its window is closed") {
                CHECK(closed == 1);
            }
        }
        host.close();
    }
}

SCENARIO("The system browser's loop ignores a close request made for an earlier loop") {
    GIVEN("a session of the first loop, kept after it ended") {
        using Browser = frontend::BrowserWithUILoopFrontend;
        // The loop's stop state is process-wide and a test before this one may have left a request pending: spend it.
        Browser::requestStop();
        Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
        std::optional<frontend::UISession> stale;
        {
            std::jthread client([&] {
                const auto executor = Browser::uiExecutor();
                (void)eventually([&] { return executor.available(); });
                stale = executor.session();
                Browser::requestStop();
            });
            Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
        }
        REQUIRE(stale);
        THEN("a later loop is not ended by it, and its own session ends it") {
            bool                staleAccepted = true;
            bool                ownAccepted   = false;
            std::jthread        client([&] {
                const auto executor = Browser::uiExecutor();
                (void)eventually([&] { return executor.available(); });
                const auto own = executor.session();
                staleAccepted  = Browser::requestClose(stale);
                ownAccepted    = eventually([&] { return Browser::requestClose(own); });
                if (!ownAccepted)
                    Browser::requestStop();
            });
            Browser::loop("127.0.0.1", "9002", "index.html", +[](const std::string&) { });
            client.join();
            CHECK_FALSE(staleAccepted);
            CHECK(ownAccepted);
        }
    }
}

#if !defined(_WIN32)
SCENARIO("The system browser's loop is up while its opener is still running") {
    GIVEN("an opener that lives longer than the close request") {
        namespace fs = std::filesystem;
        if (browser::isWSL())
            SUCCEED("WSL opens the page through PowerShell, not through a command this test can replace");
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
                closed = eventually([] { return Browser::requestClose(Browser::uiExecutor().session()); });
                if (!closed)
                    Browser::requestStop();
            });
            const auto start = std::chrono::steady_clock::now();
            Browser::open("9002", "index.html");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            host.join();
            ::setenv("PATH", saved.c_str(), 1);
            fs::remove_all(directory);
            THEN("the loop ran and ended at the request, not after the opener (#567)") {
                CHECK(closed);
                CHECK(elapsed < 2500ms);
            }
        }
    }
}

SCENARIO("The system browser opener is started without being waited for") {
    GIVEN("a command that lives for two seconds") {
        const auto start = std::chrono::steady_clock::now();
        std::mutex              mutex;
        std::condition_variable ended;
        std::optional<int>      status;
        browser::detail::spawnWithoutWaiting("sleep 2", [&](int value) {
            std::lock_guard lock(mutex);
            status = value;
            ended.notify_all();
        });
        THEN("the call returns long before it ends, and its end is reported afterwards (#567)") {
            CHECK(std::chrono::steady_clock::now() - start < 1s);
            std::unique_lock lock(mutex);
            CHECK_FALSE(status.has_value());
            REQUIRE(ended.wait_for(lock, 10s, [&] { return status.has_value(); }));
            CHECK(*status == 0);
        }
    }
    GIVEN("a command that fails") {
        std::mutex              mutex;
        std::condition_variable ended;
        std::optional<int>      status;
        browser::detail::spawnWithoutWaiting("exit 3", [&](int value) {
            std::lock_guard lock(mutex);
            status = value;
            ended.notify_all();
        });
        THEN("the status is reported, not thrown") {
            std::unique_lock lock(mutex);
            REQUIRE(ended.wait_for(lock, 10s, [&] { return status.has_value(); }));
            CHECK(WIFEXITED(*status));
            CHECK(WEXITSTATUS(*status) == 3);
        }
    }
}
#endif

SCENARIO("A window close request is always released by the executor") {
    GIVEN("a close function that holds a resource") {
        frontend::UIExecutorHost   host;
        frontend::NativeWindowHost windowHost{host.executor()};
        const auto                 resource = std::make_shared<int>(0);
        const auto                 request  = [&] {
            return frontend::detail::requestWindowClose(host.executor(), windowHost.window(), [resource](const frontend::NativeWindowHandle&) {
                ++*resource;
            });
        };
        WHEN("no loop is attached") {
            THEN("the request is refused and nothing is kept") {
                CHECK_FALSE(request());
                CHECK(resource.use_count() == 1);
            }
        }
        WHEN("the loop closes with the request pending") {
            host.attach([] { });
            REQUIRE(request());
            CHECK(resource.use_count() == 2);
            host.close();
            THEN("it completes stopped: the window is not closed and the request is freed") {
                CHECK(*resource == 0);
                CHECK(resource.use_count() == 1);
            }
        }
        WHEN("the loop runs the request") {
            host.attach([] { });
            AND_WHEN("the window is published") {
                int window = 0;
                windowHost.publish({.kind = frontend::NativeWindowKind::gtk, .pointer = &window, .portalParent = {}});
                REQUIRE(request());
                host.drain();
                THEN("the close function ran once and the request is freed") {
                    CHECK(*resource == 1);
                    CHECK(resource.use_count() == 1);
                }
            }
            AND_WHEN("no window is published") {
                REQUIRE(request());
                host.drain();
                THEN("nothing is closed and the request is freed") {
                    CHECK(*resource == 0);
                    CHECK(resource.use_count() == 1);
                }
            }
            host.close();
        }
    }
}
