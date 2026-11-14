#include "Bdd.hpp"
#include <webfront/frontend/UIExecutor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace webfront::bdd;

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
        require(changed.wait_for(lock, 30s, [&] {
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

const Scenario interfaceThread{"The interface thread runs scheduled operations", [] {
    given("a frontend loop attached to an executor", [&] {
        FakeLoop loop;
        loop.start();
        auto executor = loop.host.executor();
        require(executor.available());
        check(!executor.onUIThread());
        when("an operation is scheduled from another thread", [&] {
            Observed                observed;
            ex::inplace_stop_source stop;
            auto                    op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
            ex::start(op);
            then("it completes on the interface thread", [&] {
                require(waitFor([&] { return observed.completions == 1; }));
                check(observed.valueThread == loop.uiThread);
                check(!observed.stopped);
                check(!observed.errored);
            });
        });
        when("operations are scheduled by many threads", [&] {
            static constexpr int  threads = 4;
            static constexpr int  each    = 50;
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
            then("each one completes exactly once", [&] {
                require(waitFor([&] { return observed.completions == threads * each; }));
                check(!observed.stopped);
            });
        });
    });
}};

const Scenario operationStopped{"An operation is stopped by its token before it runs", [] {
    given("an attached loop that is not draining", [&] {
        UIExecutorHost host;
        host.attach([] { });
        auto                    executor = host.executor();
        Observed                observed;
        ex::inplace_stop_source stop;
        auto                    op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        require(observed.completions == 0);
        when("the stop is requested", [&] {
            stop.request_stop();
            then("it completes stopped and never runs", [&] {
                check(observed.completions == 1);
                check(observed.stopped);
                host.drain();
                check(observed.completions == 1);
            });
        });
        host.close();
    });
    given("a token already stopped", [&] {
        UIExecutorHost host;
        host.attach([] { });
        Observed                observed;
        ex::inplace_stop_source stop;
        stop.request_stop();
        auto op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        then("it completes stopped without queuing", [&] {
            check(observed.stopped);
            check(observed.completions == 1);
            host.drain();
            check(observed.completions == 1);
        });
        host.close();
    });
}};

const Scenario closingLoop{"Closing the loop abandons pending operations", [] {
    given("an operation pending in a loop that does not drain", [&] {
        UIExecutorHost host;
        host.attach([] { });
        auto                    executor = host.executor();
        Observed                observed;
        ex::inplace_stop_source stop;
        auto                    op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        require(observed.completions == 0);
        when("the loop closes", [&] {
            host.close();
            then("the operation completes stopped, once", [&] {
                check(observed.completions == 1);
                check(observed.stopped);
                check(!executor.available());
                host.close();  // idempotent
                check(observed.completions == 1);
            });
        });
    });
}};

const Scenario executorWithout{"An executor without a running loop is unavailable", [] {
    Observed                observed;
    ex::inplace_stop_source stop;
    given("a default handle", [&] {
        UIExecutor executor;
        check(!executor.available());
        auto op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        then("the operation fails as unavailable", [&] {
            check(observed.unavailable);
            check(observed.completions == 1);
        });
    });
    given("a host not attached yet", [&] {
        UIExecutorHost host;
        check(!host.executor().available());
        auto op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        then("the operation fails as unavailable", [&] {
            check(observed.unavailable);
        });
    });
    given("a host closed", [&] {
        UIExecutorHost host;
        host.attach([] { });
        host.close();
        auto op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        then("the operation fails as unavailable, after closure", [&] {
            check(observed.unavailable);
            check(observed.completions == 1);
        });
    });
    given("an executor whose host is destroyed", [&] {
        UIExecutor executor;
        {
            UIExecutorHost host;
            host.attach([] { });
            executor = host.executor();
        }
        check(!executor.available());
        auto op = ex::connect(executor.schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        then("the operation fails as unavailable", [&] {
            check(observed.unavailable);
        });
    });
}};

const Scenario loopCannot{"The loop cannot be attached twice and a failing wake fails the operation", [] {
    given("an attached host", [&] {
        UIExecutorHost host;
        host.attach([] { });
        checkThrows<std::logic_error>([&] { host.attach([] { }); });
        host.close();
    });
    given("a wake function that throws", [&] {
        UIExecutorHost host;
        host.attach([] { throw std::runtime_error("wake failed"); });
        Observed                observed;
        ex::inplace_stop_source stop;
        auto                    op = ex::connect(host.executor().schedule(), Receiver{observed, stop.get_token()});
        ex::start(op);
        then("the operation completes with the error and is not queued", [&] {
            check(observed.errored);
            check(observed.completions == 1);
            host.drain();
            check(observed.completions == 1);
        });
        host.close();
    });
}};

const Scenario failingWake{"A failing wake fails the operations queued behind it", [] {
    given("a wake function that queues a second operation, then throws", [&] {
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
        then("both complete with the error, and nothing is left to run", [&] {
            check(first.errored);
            check(second.errored);
            check(first.completions == 1);
            check(second.completions == 1);
            host.drain();
            check(first.completions == 1);
            check(second.completions == 1);
        });
        host.close();
    });
}};

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

const Scenario failureWhile{"A failure while queuing completes the operation and leaves the queue usable", [] {
    given("a wake function that cannot be copied", [&] {
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
        then("the operation completes with the error and nothing is queued", [&] {
            check(failed.errored);
            check(failed.completions == 1);
            check(wakes == 0);
            host.drain();
            check(failed.completions == 1);
        });
        andThen("a later operation queues, wakes the loop and runs", [&] {
            Op next(host.executor().schedule(), Receiver{later, stop.get_token()});
            next.start();
            check(wakes == 1);
            host.drain();
            check(later.completions == 1);
            check(!later.errored);
        });
        host.close();
    });
}};

const Scenario shutdownWhile{"Shutdown while operations are racing stops or runs each exactly once", [] {
    given("a loop that closes while operations are scheduled", [&] {
        for (int round = 0; round < 20; ++round) {
            FakeLoop loop;
            loop.start();
            auto                    executor = loop.host.executor();
            Observed                observed;
            ex::inplace_stop_source stop;
            static constexpr int           count = 100;
            using Op = Connected<decltype(executor.schedule())>;
            std::vector<std::unique_ptr<Op>> ops;
            for (int i = 0; i < count; ++i)
                ops.push_back(std::make_unique<Op>(executor.schedule(), Receiver{observed, stop.get_token()}));
            std::thread closer([&] { loop.stop(); });
            for (auto& op : ops)
                op->start();
            closer.join();
            require(waitFor([&] { return observed.completions == count; }));
        }
    });
}};

#include <webfront/frontend/Frontend.hpp>

#if defined(__linux__)
const Scenario linuxInterface{"The Linux interface loop coalesces wake-ups without creating deferred sources", [] {
    webfront::frontend::detail::uiloop::WakeEvent wake;
    given("a wake event shared by several producers", [&] {
        require(wake.consume() == 0);
        static constexpr int             producers = 4, signals = 10000;
        std::vector<std::jthread> threads;
        for (int i = 0; i < producers; ++i)
            threads.emplace_back([&] {
                for (int n = 0; n < signals; ++n)
                    wake.signal();
            });
        for (auto& thread : threads)
            thread.join();
        then("all signals occupy one bounded event and consumption resets it", [&] {
            check(wake.consume() == producers * signals);
            check(wake.consume() == 0);
            wake.signal();
            check(wake.consume() == 1);
        });
    });
}};
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
    check(url == "http://127.0.0.1:9002/index.html");
    ++launches;
}
}  // namespace

const Scenario systemBrowserCan{"The system browser can run an interface-thread loop on the calling thread", [] {
    using Browser = BrowserWithUILoopFrontend;
    given("the browser frontend with its loop", [&] {
        check(!Browser::uiExecutor().available());
        launches = 0;
        LoopOutcome       outcome;
        std::atomic<bool> clientOk{false};
        // No assertion off the main thread: the client records its result and always asks the loop to stop,
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
        when("open runs until the stop request", [&] {
            const auto callingThread = std::this_thread::get_id();
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            then("the page was launched, the operation ran on the calling thread and the executor is gone", [&] {
                client.join();
                check(clientOk);
                check(launches == 1);
                check(outcome.valueThread == callingThread);
                check(!outcome.stopped);
                check(!Browser::uiExecutor().available());
            });
        });
    });
    given("a host's close request while no loop runs", [&] {
        // The loop's stop state is process-wide and an earlier test may have left a request pending: spend it.
        Browser::requestStop();
        Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
        then("it reaches nothing and is not remembered for the next loop (#567)", [&] {
            check(!Browser::requestClose());
            std::jthread client([&] {
                const auto executor = Browser::uiExecutor();
                // The loop of this test ends only because the client asks, after it has run: a remembered request would end it first.
                const bool ran = waitFor([&] { return executor.available(); });
                check(ran);
                Browser::requestStop();
            });
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            client.join();
        });
        then("a request made while it runs ends it, and one made after it is refused again", [&] {
            std::atomic<bool> accepted{false};
            std::jthread      client([&] {
                const auto executor = Browser::uiExecutor();
                // The executor is available slightly before the loop can be stopped: repeat the request until accepted.
                accepted = waitFor([&] { return executor.available() && Browser::requestClose(); });
                if (!accepted)
                    Browser::requestStop();  // never leave the loop running if the request was not accepted in time
            });
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            client.join();
            check(accepted);
            check(!Browser::requestClose());
        });
    });
    given("a stop requested before the loop starts", [&] {
        Browser::requestStop();
        then("the loop ends at once", [&] {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            check(!Browser::uiExecutor().available());
        });
    });
    given("an operation queued just before the stop request", [&] {
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
        then("it completes exactly once, run or stopped, and is never left waiting", [&] {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            client.join();
            check(clientOk);
            check(outcome.completions == 1);
            check(!outcome.unavailable);
            check(!Browser::uiExecutor().available());
        });
    });
    given("several stop requests arriving while the loop exits", [&] {
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
        then("the loop ends", [&] {
            Browser::loop("127.0.0.1", "9002", "index.html", fakeLaunch);
            finished = true;
            client.join();
            check(clientOk);
            check(!Browser::uiExecutor().available());
        });
    });
#if defined(__APPLE__)
    given("a thread that is not the main thread", [&] {
        then("the loop fails before attaching anything", [&] {
            bool threw{false};
            std::thread([&] {
                try {
                    Browser::loop("127.0.0.1", "9002", "index.html", [](const std::string&) { });
                } catch (const std::runtime_error&) {
                    threw = true;
                }
            }).join();
            check(threw);
            check(!Browser::uiExecutor().available());
        });
    });
#endif
    given("a launcher that fails", [&] {
        then("no executor is left attached", [&] {
            checkThrows([&] { Browser::loop("127.0.0.1", "9002", "index.html", [](const std::string&) { throw std::runtime_error("no browser"); }); });
            check(!Browser::uiExecutor().available());
        });
    });
}};

const Scenario closingWaits{"Closing waits for the wake-ups in flight, which may use what the loop frees", [] {
    given("a wake function blocked inside an enqueue on another thread", [&] {
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
            require(changed.wait_for(lock, 30s, [&] { return inWake; }));
        }
        std::atomic<bool> closed{false};
        std::thread       closer([&] {
            host.close();
            closed = true;
        });
        when("the loop closes meanwhile", [&] {
            check(waitFor([&] { return !host.executor().available(); }));
            then("close() does not return before the wake-up is over", [&] {
                check(!closed);
                {
                    std::lock_guard lock(gate);
                    release = true;
                    changed.notify_all();
                }
                scheduler.join();
                closer.join();
                check(closed);
                check(observed.completions == 1);
                check(observed.stopped);
            });
        });
        if (scheduler.joinable()) {
            {
                std::lock_guard lock(gate);
                release = true;
                changed.notify_all();
            }
            scheduler.join();
            closer.join();
        }
    });
}};

const Scenario disarmingStop{"Disarming a stop waits for the request in flight", [] {
    given("a quit function blocked inside request() on another thread", [&] {
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
            require(changed.wait_for(lock, 30s, [&] { return inQuit; }));
        }
        std::atomic<bool> disarmed{false};
        std::thread       loop([&] {
            stop.disarm();
            disarmed = true;
        });
        std::this_thread::sleep_for(50ms);
        check(!disarmed);
        {
            std::lock_guard lock(gate);
            release = true;
            changed.notify_all();
        }
        requester.join();
        loop.join();
        check(disarmed);
        check(!stop.requestedStop());
    });
}};

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

const Scenario passInterface{"A pass of the interface thread runs a bounded batch", [] {
    given("an operation that keeps rescheduling itself", [&] {
        UIExecutorHost host;
        std::size_t    wakes{0};
        host.attach([&] { ++wakes; });  // called on this thread only: the chain starts and runs here
        Chain chain;
        chain.executor = host.executor();
        chain.limit    = 10'000;
        chain.next();
        require(wakes == 1);
        when("the loop drains once", [&] {
            host.drain();
            then("only the operations queued before the pass ran, and the loop is woken for the rest", [&] {
                check(chain.completed == 1);
                check(wakes == 2);
            });
        });
        when("the loop keeps draining after each wake", [&] {
            std::size_t passes{0};
            while (chain.completed < chain.limit && passes < 20'000) {
                host.drain();
                ++passes;
            }
            then("every operation ran, one per pass, and the loop got the thread back in between", [&] {
                check(chain.completed == chain.limit);
                check(passes == chain.limit);
                check(wakes <= passes + 1);  // one notification per pass: they never pile up in the native queue
            });
        });
        host.close();
    });
}};
