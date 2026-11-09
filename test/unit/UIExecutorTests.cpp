#include <catch2/catch_test_macros.hpp>
#include <webfront/frontend/UIExecutor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace webfront::frontend;
namespace ex = webfront::networking::ex;
using namespace std::chrono_literals;

struct Observed {
    std::atomic<int>      completions{0};
    std::atomic<bool>     stopped{false};
    std::atomic<bool>     errored{false};
    std::atomic<bool>     unavailable{false};
    std::thread::id       valueThread;
    std::function<void()> onComplete;
    void                  done() noexcept {
        if (onComplete)
            onComplete();
        ++completions;
    }
};

struct Receiver {
    using receiver_concept = ex::receiver_tag;
    Observed&              observed;
    ex::inplace_stop_token token;
    auto                   get_env() const noexcept {
        return ex::prop{ex::get_stop_token, token};
    }
    void set_value() && noexcept {
        observed.valueThread = std::this_thread::get_id();
        observed.done();
    }
    void set_stopped() && noexcept {
        observed.stopped = true;
        observed.done();
    }
    void set_error(std::exception_ptr e) && noexcept {
        observed.errored = true;
        try {
            std::rethrow_exception(e);
        } catch (const UIExecutorUnavailable&) {
            observed.unavailable = true;
        } catch (...) {
        }
        observed.done();
    }
};

// A frontend's loop: wakes are counted, and an interface thread drains when asked.
struct FakeLoop {
    UIExecutorHost          host;
    std::mutex              mutex;
    std::condition_variable changed;
    bool                    woken{false};
    bool                    quit{false};
    std::thread::id         uiThread;
    std::atomic<bool>       attached{false};
    std::thread             thread;

    void start() {
        thread = std::thread([this] {
            uiThread = std::this_thread::get_id();
            host.attach([this] {
                std::lock_guard lock(mutex);
                woken = true;
                changed.notify_all();
            });
            attached = true;
            for (;;) {
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return woken || quit; });
                    if (quit)
                        break;
                    woken = false;
                }
                host.drain();
            }
            host.close();
        });
        while (!attached)
            std::this_thread::sleep_for(1ms);
    }
    void stop() {
        {
            std::lock_guard lock(mutex);
            quit = true;
            changed.notify_all();
        }
        if (thread.joinable())
            thread.join();
    }
    ~FakeLoop() {
        stop();
    }
};

// Operation states cannot move: construct them in place, from the prvalue connect() returns.
template <typename Sender>
struct Connected {
    using Op = decltype(ex::connect(std::declval<Sender>(), std::declval<Receiver>()));
    Op op;
    Connected(Sender sender, Receiver receiver) : op(ex::connect(std::move(sender), std::move(receiver))) { }
    void start() noexcept {
        ex::start(op);
    }
};

template <typename Pred>
bool waitFor(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
}  // namespace

SCENARIO("The interface thread runs scheduled operations") {
    GIVEN("a frontend loop attached to an executor") {
        FakeLoop loop;
        loop.start();
        auto executor = loop.host.executor();
        REQUIRE(executor.available());
        CHECK_FALSE(executor.onUIThread());
        WHEN("an operation is scheduled from another thread") {
            Observed                observed;
            ex::inplace_stop_source stop;
            auto                    op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
            ex::start(op);
            THEN("it completes on the interface thread") {
                REQUIRE(waitFor([&] { return observed.completions == 1; }));
                CHECK(observed.valueThread == loop.uiThread);
                CHECK_FALSE(observed.stopped);
                CHECK_FALSE(observed.errored);
            }
        }
        WHEN("operations are scheduled by many threads") {
            constexpr int  threads = 4;
            constexpr int  each    = 50;
            Observed       observed;
            std::mutex     keep;
            std::vector<std::unique_ptr<std::thread>> workers;
            ex::inplace_stop_source                    stop;
            std::vector<std::shared_ptr<Connected<decltype(executor.schedule())>>> operations;
            for (int t = 0; t < threads; ++t)
                workers.push_back(std::make_unique<std::thread>([&] {
                    for (int i = 0; i < each; ++i) {
                        auto op = std::make_shared<Connected<decltype(executor.schedule())>>(executor.schedule(), Receiver{observed, stop.get_token()});
                        {
                            std::lock_guard lock(keep);
                            operations.push_back(op);
                        }
                        op->start();
                    }
                }));
            for (auto& worker : workers)
                worker->join();
            THEN("each one completes exactly once") {
                REQUIRE(waitFor([&] { return observed.completions == threads * each; }));
                CHECK_FALSE(observed.stopped);
            }
        }
    }
}

SCENARIO("An operation is stopped by its token before it runs") {
    GIVEN("an attached loop that is not draining") {
        UIExecutorHost host;
        host.attach([] { });
        auto                    executor = host.executor();
        Observed                observed;
        ex::inplace_stop_source stop;
        auto                    op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        REQUIRE(observed.completions == 0);
        WHEN("the stop is requested") {
            stop.request_stop();
            THEN("it completes stopped and never runs") {
                CHECK(observed.completions == 1);
                CHECK(observed.stopped);
                host.drain();
                CHECK(observed.completions == 1);
            }
        }
        host.close();
    }
    GIVEN("a token already stopped") {
        UIExecutorHost host;
        host.attach([] { });
        Observed                observed;
        ex::inplace_stop_source stop;
        stop.request_stop();
        auto op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        THEN("it completes stopped without queuing") {
            CHECK(observed.stopped);
            CHECK(observed.completions == 1);
            host.drain();
            CHECK(observed.completions == 1);
        }
        host.close();
    }
}

SCENARIO("Closing the loop abandons pending operations") {
    GIVEN("an operation pending in a loop that does not drain") {
        UIExecutorHost host;
        host.attach([] { });
        auto                    executor = host.executor();
        Observed                observed;
        ex::inplace_stop_source stop;
        auto                    op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        REQUIRE(observed.completions == 0);
        WHEN("the loop closes") {
            host.close();
            THEN("the operation completes stopped, once") {
                CHECK(observed.completions == 1);
                CHECK(observed.stopped);
                CHECK_FALSE(executor.available());
                host.close();  // idempotent
                CHECK(observed.completions == 1);
            }
        }
    }
}

SCENARIO("An executor without a running loop is unavailable") {
    Observed                observed;
    ex::inplace_stop_source stop;
    GIVEN("a default handle") {
        UIExecutor executor;
        CHECK_FALSE(executor.available());
        auto op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        THEN("the operation fails as unavailable") {
            CHECK(observed.unavailable);
            CHECK(observed.completions == 1);
        }
    }
    GIVEN("a host not attached yet") {
        UIExecutorHost host;
        CHECK_FALSE(host.executor().available());
        auto op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        THEN("the operation fails as unavailable") {
            CHECK(observed.unavailable);
        }
    }
    GIVEN("a host closed") {
        UIExecutorHost host;
        host.attach([] { });
        host.close();
        auto op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        THEN("the operation fails as unavailable, after closure") {
            CHECK(observed.unavailable);
            CHECK(observed.completions == 1);
        }
    }
    GIVEN("an executor whose host is destroyed") {
        UIExecutor executor;
        {
            UIExecutorHost host;
            host.attach([] { });
            executor = host.executor();
        }
        CHECK_FALSE(executor.available());
        auto op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        THEN("the operation fails as unavailable") {
            CHECK(observed.unavailable);
        }
    }
}

SCENARIO("The loop cannot be attached twice and a failing wake fails the operation") {
    GIVEN("an attached host") {
        UIExecutorHost host;
        host.attach([] { });
        CHECK_THROWS_AS(host.attach([] { }), std::logic_error);
        host.close();
    }
    GIVEN("a wake function that throws") {
        UIExecutorHost host;
        host.attach([] { throw std::runtime_error("wake failed"); });
        Observed                observed;
        ex::inplace_stop_source stop;
        auto                    op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        THEN("the operation completes with the error and is not queued") {
            CHECK(observed.errored);
            CHECK(observed.completions == 1);
            host.drain();
            CHECK(observed.completions == 1);
        }
        host.close();
    }
}

SCENARIO("Shutdown while operations are racing stops or runs each exactly once") {
    GIVEN("a loop that closes while operations are scheduled") {
        for (int round = 0; round < 20; ++round) {
            FakeLoop loop;
            loop.start();
            auto                    executor = loop.host.executor();
            Observed                observed;
            ex::inplace_stop_source stop;
            constexpr int           count = 100;
            using Op = Connected<decltype(executor.schedule())>;
            std::vector<std::unique_ptr<Op>> ops;
            for (int i = 0; i < count; ++i)
                ops.push_back(std::make_unique<Op>(executor.schedule(), Receiver{observed, stop.get_token()}));
            std::thread closer([&] { loop.stop(); });
            for (auto& op : ops)
                op->start();
            closer.join();
            REQUIRE(waitFor([&] { return observed.completions == count; }));
        }
    }
}

#include <webfront/frontend/Frontend.hpp>

namespace {
struct LoopOutcome {
    std::atomic<int>  completions{0};
    std::atomic<bool> stopped{false};
    std::atomic<bool> unavailable{false};
    std::thread::id   valueThread;
};
struct LoopReceiver {
    using receiver_concept = ex::receiver_tag;
    LoopOutcome& outcome;
    auto         get_env() const noexcept {
        return ex::prop{ex::get_stop_token, ex::never_stop_token{}};
    }
    void set_value() && noexcept {
        outcome.valueThread = std::this_thread::get_id();
        ++outcome.completions;
    }
    void set_stopped() && noexcept {
        outcome.stopped = true;
        ++outcome.completions;
    }
    void set_error(std::exception_ptr) && noexcept {
        outcome.unavailable = true;
        ++outcome.completions;
    }
};
std::atomic<int> launches{0};
void             fakeLaunch(const std::string& url) {
    CHECK(url == "http://127.0.0.1:9002/index.html");
    ++launches;
}
}  // namespace

SCENARIO("The system browser can run an interface-thread loop on the calling thread") {
    using Browser = BrowserWithUILoopFrontend;
    GIVEN("the browser frontend with its loop") {
        CHECK_FALSE(Browser::uiExecutor().available());
        launches = 0;
        LoopOutcome outcome;
        std::jthread client([&] {
            const auto executor = Browser::uiExecutor();
            REQUIRE(waitFor([&] { return executor.available(); }));
            using Op = Connected<decltype(executor.schedule())>;
            // A function-local receiver type keeps the helper simple: schedule one operation and wait for it.
            struct Holder {
                decltype(ex::connect(std::declval<decltype(executor.schedule())>(), std::declval<LoopReceiver>())) op;
                Holder(const UIExecutor& e, LoopOutcome& o) : op(ex::connect(e.schedule(), LoopReceiver{o})) { }
            } holder(executor, outcome);
            ex::start(holder.op);
            REQUIRE(waitFor([&] { return outcome.completions == 1; }));
            Browser::requestStop();
            (void)sizeof(Op);
        });
        WHEN("open runs until the stop request") {
            const auto callingThread = std::this_thread::get_id();
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            THEN("the page was launched, the operation ran on the calling thread and the executor is gone") {
                client.join();
                CHECK(launches == 1);
                CHECK(outcome.valueThread == callingThread);
                CHECK_FALSE(outcome.stopped);
                CHECK_FALSE(Browser::uiExecutor().available());
            }
        }
    }
    GIVEN("a stop requested before the loop starts") {
        Browser::requestStop();
        THEN("the loop ends at once") {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            CHECK_FALSE(Browser::uiExecutor().available());
        }
    }
    GIVEN("several stop requests arriving while the loop exits") {
        launches = 0;
        std::atomic<bool> finished{false};
        std::jthread     client([&] {
            const auto executor = Browser::uiExecutor();
            REQUIRE(waitFor([&] { return executor.available(); }));
            // Each request may queue a source in the platform loop: none may outlive it or touch what it freed.
            while (!finished)
                Browser::requestStop();
        });
        THEN("the loop ends") {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            finished = true;
            client.join();
            CHECK_FALSE(Browser::uiExecutor().available());
        }
    }
    GIVEN("a launcher that fails") {
        THEN("no executor is left attached") {
            CHECK_THROWS(Browser::loop("127.0.0.1", "9002", "index.html", [](const std::string&) { throw std::runtime_error("no browser"); }));
            CHECK_FALSE(Browser::uiExecutor().available());
        }
    }
}

SCENARIO("Closing waits for the wake-ups in flight, which may use what the loop frees") {
    GIVEN("a wake function blocked inside an enqueue on another thread") {
        UIExecutorHost          host;
        std::mutex              gate;
        std::condition_variable changed;
        bool                    inWake{false};
        bool                    release{false};
        host.attach([&] {
            std::unique_lock lock(gate);
            inWake = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        Observed                observed;
        ex::inplace_stop_source stop;
        using Op = Connected<decltype(host.executor().schedule())>;
        auto op  = std::make_unique<Op>(host.executor().schedule(), Receiver{observed, stop.get_token()});
        std::thread scheduler([&] { op->start(); });
        {
            std::unique_lock lock(gate);
            REQUIRE(changed.wait_for(lock, 30s, [&] { return inWake; }));
        }
        std::atomic<bool> closed{false};
        std::thread       closer([&] {
            host.close();
            closed = true;
        });
        WHEN("the loop closes meanwhile") {
            std::this_thread::sleep_for(50ms);
            THEN("close() does not return before the wake-up is over") {
                CHECK_FALSE(closed);
                {
                    std::lock_guard lock(gate);
                    release = true;
                    changed.notify_all();
                }
                scheduler.join();
                closer.join();
                CHECK(closed);
                CHECK(observed.completions == 1);
                CHECK(observed.stopped);
            }
        }
        if (scheduler.joinable()) {
            {
                std::lock_guard lock(gate);
                release = true;
                changed.notify_all();
            }
            scheduler.join();
            closer.join();
        }
    }
}

SCENARIO("Disarming a stop waits for the request in flight") {
    GIVEN("a quit function blocked inside request() on another thread") {
        UILoopStop              stop;
        std::mutex              gate;
        std::condition_variable changed;
        bool                    inQuit{false};
        bool                    release{false};
        stop.arm([&] {
            std::unique_lock lock(gate);
            inQuit = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        std::thread requester([&] { stop.request(); });
        {
            std::unique_lock lock(gate);
            REQUIRE(changed.wait_for(lock, 30s, [&] { return inQuit; }));
        }
        std::atomic<bool> disarmed{false};
        std::thread       loop([&] {
            stop.disarm();
            disarmed = true;
        });
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(disarmed);
        {
            std::lock_guard lock(gate);
            release = true;
            changed.notify_all();
        }
        requester.join();
        loop.join();
        CHECK(disarmed);
        CHECK_FALSE(stop.requestedStop());
    }
}
