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
    bool                    attached{false};
    std::thread             thread;

    void start() {
        thread = std::thread([this] {
            uiThread = std::this_thread::get_id();
            host.attach([this] {
                std::lock_guard lock(mutex);
                woken = true;
                changed.notify_all();
            });
            {
                std::lock_guard lock(mutex);
                attached = true;
                changed.notify_all();
            }
            for (;;) {
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] {
                        return woken || quit;
                    });
                    if (quit)
                        break;
                    woken = false;
                }
                host.drain();
            }
            host.close();
        });
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 30s, [&] {
            return attached;
        }));
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

SCENARIO("A failing wake fails the operations queued behind it") {
    GIVEN("a wake function that queues a second operation, then throws") {
        UIExecutorHost          host;
        Observed                first;
        Observed                second;
        ex::inplace_stop_source stop;
        using Op = Connected<decltype(host.executor().schedule())>;
        Op                 secondOp(host.executor().schedule(), Receiver{second, stop.get_token()});
        host.attach([&] {
            secondOp.start();  // queued while the notification of the first is pending: it asks for none
            throw std::runtime_error("wake failed");
        });
        Op firstOp(host.executor().schedule(), Receiver{first, stop.get_token()});
        firstOp.start();
        THEN("both complete with the error, and nothing is left to run") {
            CHECK(first.errored);
            CHECK(second.errored);
            CHECK(first.completions == 1);
            CHECK(second.completions == 1);
            host.drain();
            CHECK(first.completions == 1);
            CHECK(second.completions == 1);
        }
        host.close();
    }
}

namespace {
// A wake function whose copy can be made to fail, as an allocation would.
struct FragileWake {
    static inline std::atomic<bool> failCopy{false};
    std::size_t*                    wakes;
    explicit FragileWake(std::size_t* count) : wakes(count) { }
    FragileWake(const FragileWake& other) : wakes(other.wakes) {
        if (failCopy)
            throw std::bad_alloc();
    }
    FragileWake(FragileWake&&) noexcept = default;
    void operator()() const {
        ++*wakes;
    }
};
}  // namespace

SCENARIO("A failure while queuing completes the operation and leaves the queue usable") {
    GIVEN("a wake function that cannot be copied") {
        UIExecutorHost host;
        std::size_t    wakes{0};
        host.attach(FragileWake{&wakes});
        Observed                failed;
        Observed                later;
        ex::inplace_stop_source stop;
        using Op = Connected<decltype(host.executor().schedule())>;
        FragileWake::failCopy = true;
        Op failing(host.executor().schedule(), Receiver{failed, stop.get_token()});
        failing.start();  // start() is noexcept: the failure must not end the process
        FragileWake::failCopy = false;
        THEN("the operation completes with the error and nothing is queued") {
            CHECK(failed.errored);
            CHECK(failed.completions == 1);
            CHECK(wakes == 0);
            host.drain();
            CHECK(failed.completions == 1);
        }
        AND_THEN("a later operation queues, wakes the loop and runs") {
            Op next(host.executor().schedule(), Receiver{later, stop.get_token()});
            next.start();
            CHECK(wakes == 1);
            host.drain();
            CHECK(later.completions == 1);
            CHECK_FALSE(later.errored);
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

#if defined(__linux__)
SCENARIO("The Linux interface loop coalesces wake-ups without creating deferred sources") {
    webfront::frontend::detail::uiloop::WakeEvent wake;
    GIVEN("a wake event shared by several producers") {
        REQUIRE(wake.consume() == 0);
        constexpr int             producers = 4, signals = 10000;
        std::vector<std::jthread> threads;
        for (int i = 0; i < producers; ++i)
            threads.emplace_back([&] {
                for (int n = 0; n < signals; ++n)
                    wake.signal();
            });
        for (auto& thread : threads)
            thread.join();
        THEN("all signals occupy one bounded event and consumption resets it") {
            CHECK(wake.consume() == producers * signals);
            CHECK(wake.consume() == 0);
            wake.signal();
            CHECK(wake.consume() == 1);
        }
    }
}
#endif

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
        LoopOutcome       outcome;
        std::atomic<bool> clientOk{false};
        // No Catch2 assertion off the main thread: the client records its result and always asks the loop to stop,
        // so that a timeout fails the test instead of hanging or terminating the process.
        std::jthread client([&] {
            const auto executor = Browser::uiExecutor();
            if (waitFor([&] { return executor.available(); })) {
                // A function-local receiver type keeps the helper simple: schedule one operation and wait for it.
                struct Holder {
                    decltype(ex::connect(std::declval<decltype(executor.schedule())>(), std::declval<LoopReceiver>())) op;
                    Holder(const UIExecutor& e, LoopOutcome& o) : op(ex::connect(e.schedule(), LoopReceiver{o})) { }
                } holder(executor, outcome);
                ex::start(holder.op);
                clientOk = waitFor([&] { return outcome.completions == 1; });
            }
            Browser::requestStop();
        });
        WHEN("open runs until the stop request") {
            const auto callingThread = std::this_thread::get_id();
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            THEN("the page was launched, the operation ran on the calling thread and the executor is gone") {
                client.join();
                CHECK(clientOk);
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
    GIVEN("an operation queued just before the stop request") {
        launches = 0;
        LoopOutcome       outcome;
        std::atomic<bool> clientOk{false};
        std::jthread      client([&] {
            const auto executor = Browser::uiExecutor();
            if (waitFor([&] { return executor.available(); })) {
                struct Holder {
                    decltype(ex::connect(std::declval<decltype(Browser::uiExecutor().schedule())>(), std::declval<LoopReceiver>())) op;
                    Holder(const UIExecutor& e, LoopOutcome& o) : op(ex::connect(e.schedule(), LoopReceiver{o})) { }
                } holder(executor, outcome);
                ex::start(holder.op);
                Browser::requestStop();  // the loop may run the job first or close with it still queued
                clientOk = waitFor([&] { return outcome.completions >= 1; });
            } else {
                Browser::requestStop();
            }
        });
        THEN("it completes exactly once, run or stopped, and is never left waiting") {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            client.join();
            CHECK(clientOk);
            CHECK(outcome.completions == 1);
            CHECK_FALSE(outcome.unavailable);
            CHECK_FALSE(Browser::uiExecutor().available());
        }
    }
    GIVEN("several stop requests arriving while the loop exits") {
        launches = 0;
        std::atomic<bool> finished{false};
        std::atomic<bool> clientOk{false};
        std::jthread     client([&] {
            const auto executor = Browser::uiExecutor();
            clientOk = waitFor([&] { return executor.available(); });
            // Each request may queue a source in the platform loop: none may outlive it or touch what it freed.
            do
                Browser::requestStop();
            while (!finished);
        });
        THEN("the loop ends") {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            finished = true;
            client.join();
            CHECK(clientOk);
            CHECK_FALSE(Browser::uiExecutor().available());
        }
    }
#if defined(__APPLE__)
    GIVEN("a thread that is not the main thread") {
        THEN("the loop fails before attaching anything") {
            bool threw{false};
            std::thread([&] {
                try {
                    Browser::loop("127.0.0.1", "9002", "index.html", [](const std::string&) { });
                } catch (const std::runtime_error&) {
                    threw = true;
                }
            }).join();
            CHECK(threw);
            CHECK_FALSE(Browser::uiExecutor().available());
        }
    }
#endif
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
            CHECK(waitFor([&] { return !host.executor().available(); }));
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

namespace {
// An operation that schedules its successor from the interface thread, `limit` times.
struct Chain {
    struct Link;
    UIExecutor                         executor;
    std::size_t                        limit{0};
    std::size_t                        completed{0};
    std::vector<std::unique_ptr<Link>> links;
    void                               next();
};
struct ChainReceiver {
    using receiver_concept = ex::receiver_tag;
    Chain* chain;
    auto   get_env() const noexcept {
        return ex::prop{ex::get_stop_token, ex::never_stop_token{}};
    }
    void set_value() && noexcept {
        ++chain->completed;
        if (chain->completed < chain->limit)
            chain->next();
    }
    void set_stopped() && noexcept { }
    void set_error(std::exception_ptr) && noexcept { }
};
struct Chain::Link {
    decltype(ex::connect(std::declval<decltype(std::declval<UIExecutor>().schedule())>(), std::declval<ChainReceiver>())) op;
    Link(UIExecutor e, Chain* c) : op(ex::connect(e.schedule(), ChainReceiver{c})) { }
};
void Chain::next() {
    links.push_back(std::make_unique<Link>(executor, this));
    ex::start(links.back()->op);
}
}  // namespace

SCENARIO("A pass of the interface thread runs a bounded batch") {
    GIVEN("an operation that keeps rescheduling itself") {
        UIExecutorHost host;
        std::size_t    wakes{0};
        host.attach([&] { ++wakes; });  // called on this thread only: the chain starts and runs here
        Chain chain;
        chain.executor = host.executor();
        chain.limit    = 10'000;
        chain.next();
        REQUIRE(wakes == 1);
        WHEN("the loop drains once") {
            host.drain();
            THEN("only the operations queued before the pass ran, and the loop is woken for the rest") {
                CHECK(chain.completed == 1);
                CHECK(wakes == 2);
            }
        }
        WHEN("the loop keeps draining after each wake") {
            std::size_t passes{0};
            while (chain.completed < chain.limit && passes < 20'000) {
                host.drain();
                ++passes;
            }
            THEN("every operation ran, one per pass, and the loop got the thread back in between") {
                CHECK(chain.completed == chain.limit);
                CHECK(passes == chain.limit);
                CHECK(wakes <= passes + 1);  // one notification per pass: they never pile up in the native queue
            }
        }
        host.close();
    }
}
