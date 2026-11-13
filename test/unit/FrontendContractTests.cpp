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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

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
    GIVEN("a frontend whose launch takes the content policy") {
        PolicyLaunchRecorder::receivedPolicy = false;
        HostApp<PolicyLaunchRecorder> app;
        app.start();
        THEN("start gives it the application's policy") {
            CHECK(PolicyLaunchRecorder::receivedPolicy);
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
