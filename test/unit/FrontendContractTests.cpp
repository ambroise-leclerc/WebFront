#include "Mocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/WebFront.hpp>

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
    using App = BasicWFWithFrontend<ContractNet, MockFileSystem<>, ContractFrontend<onReturn, failure>>;
    App                app;
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
    using App = BasicWFWithFrontend<ContractNet, MockFileSystem<>, ContractFrontend<onReturn, Failure::beforeWindow>>;
    App        app;
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
