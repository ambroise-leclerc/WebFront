#include "InjectableNetworking.hpp"
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace webfront::bdd;

namespace {
namespace ex = webfront::networking::ex;
using App    = webfront::BasicWF<webfront::networking::NetworkingMock, webfront::fs::DefaultFS>;

webfront::Task finishedTask(bool& ran) {
    ran = true;
    co_return;
}

webfront::Task pendingTask(std::shared_ptr<int> lifetime) {
    static_cast<void>(lifetime);
    co_await webfront::testing::ManualSender([](auto) { });
}

struct ExternalSender {
    using sender_concept        = ex::sender_tag;
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    std::function<void()>& complete;
    template <typename Receiver>
    struct Operation {
        Receiver               receiver;
        std::function<void()>& complete;
        void                   start() noexcept {
            complete = [this] {
                ex::set_value(std::move(receiver));
            };
        }
    };
    template <typename Receiver>
    auto connect(Receiver receiver) const {
        return Operation<Receiver>{std::move(receiver), complete};
    }
};
webfront::Task externalTask(std::function<void()>& complete) {
    co_await ExternalSender{complete};
}

struct PausedStartSender {
    using sender_concept        = ex::sender_tag;
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    std::latch& started;
    std::latch& release;
    bool        failConnection = false;

    template <typename Receiver>
    struct Operation {
        Receiver    receiver;
        std::latch& started;
        std::latch& release;
        void        start() noexcept {
            started.count_down();
            release.wait();
            ex::set_value(std::move(receiver));
        }
    };
    template <typename Receiver>
    auto connect(Receiver receiver) const {
        if (failConnection)
            throw std::runtime_error("spawn connection failed");
        return Operation<Receiver>{std::move(receiver), started, release};
    }
};
}  // namespace

const Scenario scopeDrain{"Scope drain waits for a concurrent spawn to finish starting", [] {
    webfront::networking::TaskScope     scope;
    webfront::networking::IoContextMock context;
    std::latch                          started(1), release(1);
    given("a task associated with the scope but paused inside start", [&] {
        std::atomic<bool> accepted{false};
        std::jthread      caller([&] {
            accepted = scope.spawn(PausedStartSender{started, release});
        });
        started.wait();
        std::latch stopping(1);
        auto       onStop = [&] {
            stopping.count_down();
        };
        ex::stop_callback_for_t<ex::inplace_stop_token, decltype(onStop)> callback(scope.stopToken(), onStop);
        std::promise<void>                                                result;
        auto                                                              finished = result.get_future();
        std::jthread                                                      drainer([&] {
            try {
                scope.drain(context);
                result.set_value();
            } catch (...) {
                result.set_exception(std::current_exception());
            }
        });
        stopping.wait();
        const auto status = finished.wait_for(std::chrono::milliseconds(100));
        release.count_down();
        caller.join();
        drainer.join();
        then("the empty context is not mistaken for a stalled task", [&] {
            check(accepted.load());
            check(status == std::future_status::timeout);
            checkNothrow([&] { finished.get(); });
            checkNothrow([&] { scope.drain(context); });
        });
    });
    given("a sender that throws while connecting", [&] {
        checkThrows<std::runtime_error>([&] { scope.spawn(PausedStartSender{started, release, true}); });
        then("draining does not wait for the failed spawn", [&] {
            checkNothrow([&] { scope.drain(context); });
        });
    });
}};

const Scenario applicationsOwn{"Applications own spawned coroutines and expose individual cancellation", [] {
    given("two application coroutines suspended on cancellable operations", [&] {
        App                app(webfront::WebFrontConfig{.port = "0"});
        auto               first       = std::make_shared<int>(1);
        auto               second      = std::make_shared<int>(2);
        std::weak_ptr<int> firstFrame  = first;
        std::weak_ptr<int> secondFrame = second;
        auto               handle      = app.spawn(pendingTask(std::move(first)));
        App::UI            ui(app, 0, 1);
        ui.spawn(pendingTask(std::move(second)));
        require(!firstFrame.expired());
        require(!secondFrame.expired());
        when("one task is cancelled through its handle", [&] {
            handle.requestStop();
            then("only that coroutine unwinds", [&] {
                check(firstFrame.expired());
                check(!secondFrame.expired());
                check(handle.stopRequested());
            });
        });
        when("the application shuts down", [&] {
            app.shutdown();
            then("both frames are destroyed before it returns", [&] {
                check(firstFrame.expired());
                check(secondFrame.expired());
                checkNothrow([&] { app.shutdown(); });
                bool ran      = false;
                auto rejected = app.spawn(finishedTask(ran));
                check(rejected.stopRequested());
                check(!ran);
            });
        });
    });
    given("a coroutine that completes normally", [&] {
        App  app(webfront::WebFrontConfig{.port = "0"});
        bool ran    = false;
        auto handle = app.spawn(finishedTask(ran));
        then("it runs without an explicit scope or context", [&] {
            check(ran);
            check(!handle.stopRequested());
        });
    });
    given("a handle retained beyond its application", [&] {
        webfront::TaskHandle handle;
        std::weak_ptr<int>   frame;
        {
            App  app(webfront::WebFrontConfig{.port = "0"});
            auto lifetime = std::make_shared<int>(42);
            frame         = lifetime;
            handle        = app.spawn(pendingTask(std::move(lifetime)));
        }
        then("destruction drained its task and the handle remains safe", [&] {
            check(frame.expired());
            checkNothrow([&] { handle.requestStop(); });
        });
    });
}};

const Scenario stalledScope{"A stalled scope reports absent context work without abandoning its join", [] {
    given("a task suspended on an external operation that ignores cancellation", [&] {
        std::function<void()>               complete;
        webfront::testing::InlineTaskScope  scope;
        webfront::networking::IoContextMock context;
        webfront::networking::Spawn(scope, ex::inline_scheduler{}, externalTask(complete));
        when("drain finds no context completion capable of advancing the task", [&] {
            checkThrows<std::logic_error>([&] { scope.drain(context); });
            then("the join remains alive and can finish after external completion", [&] {
                require(complete);
                complete();
                checkNothrow([&] { scope.drain(context); });
            });
        });
    });
}};

namespace {
struct SpawnOnExit {
    App&                                 app;
    bool&                                ran;
    std::optional<webfront::TaskHandle>& handle;
    ~SpawnOnExit() {
        handle = app.spawn(finishedTask(ran));
    }
};
webfront::Task spawnDuringCleanup(App& app, bool& ran, std::optional<webfront::TaskHandle>& handle) {
    SpawnOnExit guard{app, ran, handle};
    co_await webfront::testing::ManualSender([](auto) { });
}
}  // namespace

const Scenario spawningDuring{"Spawning during shutdown is rejected with a stopped handle", [] {
    given("a coroutine whose cleanup tries to start another task", [&] {
        bool                                ran = false;
        std::optional<webfront::TaskHandle> handle;
        App                                 app(webfront::WebFrontConfig{.port = "0"});
        app.spawn(spawnDuringCleanup(app, ran, handle));
        when("the application stops its existing tasks", [&] {
            checkNothrow([&] { app.shutdown(); });
            then("the nested spawn does not throw or start its body", [&] {
                require(handle);
                check(handle->stopRequested());
                check(!ran);
            });
        });
    });
}};

const Scenario spawnScope{"Spawn and scope close agree on whether work was accepted", [] {
    given("a scope closed before any stop request", [&] {
        bool                               ran = false;
        webfront::testing::InlineTaskScope scope;
        scope.close();
        require(!scope.stopRequested());
        when("a cold task is offered to it", [&] {
            auto accepted = webfront::networking::Spawn(scope, ex::inline_scheduler{}, finishedTask(ran));
            then("association rejection is reported without executing the task", [&] {
                check(!accepted);
                check(!ran);
            });
        });
    });

    given("a caller starting tasks while another thread shuts the application down", [&] {
        App               app(webfront::WebFrontConfig{.port = "0"});
        std::atomic<bool> go{false};
        std::atomic<bool> invalid{false};
        std::jthread      caller([&] {
            go.wait(false);
            for (int i = 0; i != 100; ++i) {
                bool ran    = false;
                auto handle = app.spawn(finishedTask(ran));
                if (!ran && !handle.stopRequested())
                    invalid = true;
            }
        });
        go = true;
        go.notify_one();
        checkNothrow([&] { app.shutdown(); });
        caller.join();
        then("every rejected task returns a stopped handle", [&] {
            check(!invalid.load());
        });
    });
}};

const Scenario destructionWaits{"Destruction waits safely after an explicit shutdown reports a pending external task", [] {
    given("a task that cannot finish until an external operation completes", [&] {
        std::function<void()> complete;
        auto                  app = std::make_unique<App>(webfront::WebFrontConfig{.port = "0"});
        app->spawn(externalTask(complete));
        checkThrows<std::logic_error>([&] { app->shutdown(); });
        require(complete);
        std::jthread finishing([complete] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            complete();
        });
        when("the application is destroyed before that completion arrives", [&] {
            app.reset();
            finishing.join();
            then("its destructor joins instead of allowing a drain exception to escape", [&] {
                check(!app);
            });
        });
    });
}};
