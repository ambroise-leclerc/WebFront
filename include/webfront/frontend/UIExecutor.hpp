/// @brief Execution on the interface thread of a frontend (ADR-020, prerequisite of the native dialogs).
///
/// A frontend that owns an interface thread publishes a `UIExecutorHost`: while its loop runs, it calls `attach()` on the
/// interface thread with a thread-safe `wake` function, then `drain()` each time `wake` was called, and `close()` when the
/// loop ends. Everything else uses the `UIExecutor` handle, whose `schedule()` is a sender that completes on the interface
/// thread (ADR-006):
///  - `set_value()` on the interface thread, once the host drains it;
///  - `set_stopped()` when the receiver's stop token fires before the operation started to run, or when the loop closes
///    with the operation still pending: it never runs after closure;
///  - `set_error(UIExecutorUnavailable)` when no loop is attached (system browser without the loop mode, loop not started
///    yet, or already closed).
/// An operation that started to run is never interrupted: stop requests arrive too late for it.
#pragma once

#include "../networking/Tasks.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::frontend {
namespace ex = networking::ex;

/// @brief No interface-thread loop is attached: the frontend has none, it has not started or it has closed.
class UIExecutorUnavailable : public std::runtime_error {
public:
    UIExecutorUnavailable() : std::runtime_error("no interface thread is available: the frontend has no running loop") { }
};

namespace detail {

/// One pending operation. The first of run / stop / close / failure to claim it completes the receiver, once.
struct UIJob {
    virtual ~UIJob()                         = default;
    virtual void run() noexcept              = 0;  // on the interface thread
    virtual void abandon() noexcept          = 0;  // loop closed or stop requested: completes stopped
    virtual void fail(std::exception_ptr) noexcept = 0;
};

class UIExecutorShared {
public:
    enum class Phase { idle, running, closed };

    /// Ends one wake-up in flight, also when the wake function throws.
    struct Done {
        UIExecutorShared& self;
        ~Done() {
            std::lock_guard lock(self.mutex);
            --self.wakesInFlight;
            self.wakesDone.notify_all();
        }
    };

    /// Queues `job` and wakes the loop; false (job untouched) when no loop is running.
    bool enqueue(const std::shared_ptr<UIJob>& job) {
        std::function<void()> wake;
        {
            std::lock_guard lock(mutex);
            if (phase != Phase::running)
                return false;
            queue.push_back(job);
            // One notification per pass: while one is pending, the next drain() will see this job too.
            if (!wakePending) {
                wakePending = true;
                wake        = wakeFunction;
                ++wakesInFlight;  // close() waits for it: the wake may use what the loop frees once it has closed
            }
        }
        if (!wake)
            return true;
        try {
            Done done{*this};
            wake();
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                wakePending = false;
            }
            remove(job);
            job->fail(std::current_exception());
        }
        return true;
    }

    void remove(const std::shared_ptr<UIJob>& job) {
        std::lock_guard lock(mutex);
        std::erase(queue, job);
    }

    void attach(std::function<void()> wake) {
        std::lock_guard lock(mutex);
        if (phase == Phase::running)
            throw std::logic_error("UIExecutorHost::attach: a loop is already attached");
        wakeFunction = std::move(wake);
        wakePending  = false;
        uiThread     = std::this_thread::get_id();
        phase        = Phase::running;
    }

    /// Runs the operations queued when it starts, no more: one that reschedules itself, or a producer that keeps queueing,
    /// runs in a later pass so that the native loop handles its events (closing, stop requests) in between. When work
    /// remains, the loop is woken again.
    void drain() noexcept {
        std::size_t batch{};
        {
            std::lock_guard lock(mutex);
            wakePending = false;  // from here on a new job asks for a new pass
            batch       = queue.size();
        }
        for (; batch > 0; --batch) {
            std::shared_ptr<UIJob> job;
            {
                std::lock_guard lock(mutex);
                if (queue.empty())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            job->run();
        }
        std::function<void()> wake;
        {
            std::lock_guard lock(mutex);
            if (queue.empty() || phase != Phase::running || wakePending)
                return;  // nothing left, or a job queued meanwhile already asked for the next pass
            wakePending = true;
            wake        = wakeFunction;
            ++wakesInFlight;
        }
        // A wake that throws leaves the rest for the next one: any later schedule() wakes the loop again.
        try {
            Done done{*this};
            wake();
        } catch (...) {
            std::lock_guard lock(mutex);
            wakePending = false;
        }
    }

    void close() noexcept {
        std::deque<std::shared_ptr<UIJob>> pending;
        {
            std::unique_lock lock(mutex);
            phase = Phase::closed;
            pending.swap(queue);
            // No new wake starts (phase is closed): wait for those in flight on other threads, which only signal the loop.
            wakesDone.wait(lock, [&] { return wakesInFlight == 0; });
            wakeFunction = nullptr;  // may hold the session that is ending
        }
        for (auto& job : pending)
            job->abandon();
    }

    [[nodiscard]] bool running() const {
        std::lock_guard lock(mutex);
        return phase == Phase::running;
    }
    [[nodiscard]] bool onUIThread() const {
        std::lock_guard lock(mutex);
        return phase == Phase::running && uiThread == std::this_thread::get_id();
    }

private:
    mutable std::mutex                 mutex;
    std::condition_variable            wakesDone;
    unsigned                           wakesInFlight{0};
    bool                               wakePending{false};
    Phase                              phase{Phase::idle};
    std::thread::id                    uiThread;
    std::function<void()>              wakeFunction;
    std::deque<std::shared_ptr<UIJob>> queue;
};

class UIScheduleSender {
    std::weak_ptr<UIExecutorShared> target;

    template <typename Receiver>
    struct State final : UIJob, std::enable_shared_from_this<State<Receiver>> {
        using Token = ex::stop_token_of_t<ex::env_of_t<Receiver>>;
        struct OnStop {
            std::weak_ptr<State> state;
            void                 operator()() noexcept {
                if (auto self = state.lock())
                    self->abandon();
            }
        };
        enum class Outcome { pending, value, stopped, error };

        Receiver                                              receiver;
        std::weak_ptr<UIExecutorShared>                       owner;
        std::mutex                                            mutex;
        bool                                                  starting{true};
        Outcome                                               outcome{Outcome::pending};
        std::exception_ptr                                    failure;
        std::optional<ex::stop_callback_for_t<Token, OnStop>> stopCallback;

        State(Receiver r, std::weak_ptr<UIExecutorShared> o) : receiver(std::move(r)), owner(std::move(o)) { }

        // The first completion wins. Delivery is deferred while start() registers the stop callback: a token already
        // stopped invokes it inline, and the callback is not yet constructed.
        void finish(Outcome value, std::exception_ptr error = {}) noexcept {
            bool ready{};
            {
                std::lock_guard lock(mutex);
                if (outcome != Outcome::pending)
                    return;
                outcome = value;
                failure = std::move(error);
                ready   = !starting;
            }
            if (ready)
                deliver();
        }

        void start() noexcept {
            auto self   = this->shared_from_this();
            auto shared = owner.lock();
            try {
                stopCallback.emplace(ex::get_stop_token(ex::get_env(receiver)), OnStop{this->weak_from_this()});
            } catch (...) {
                finish(Outcome::error, std::current_exception());
            }
            // From here on a completion is delivered by whoever wins it: the interface thread for a run, so that a receiver
            // sees set_value() there even when the loop drains the job before start() returns.
            bool ready{};
            {
                std::lock_guard lock(mutex);
                starting = false;
                ready    = outcome != Outcome::pending;
            }
            if (ready) {
                deliver();
                return;
            }
            if (!shared || !shared->enqueue(self))
                finish(Outcome::error, std::make_exception_ptr(UIExecutorUnavailable{}));
        }

        void run() noexcept override {
            finish(Outcome::value);
        }
        void abandon() noexcept override {
            if (auto shared = owner.lock())
                shared->remove(this->shared_from_this());
            finish(Outcome::stopped);
        }
        void fail(std::exception_ptr error) noexcept override {
            finish(Outcome::error, std::move(error));
        }

        void deliver() noexcept {
            auto self = this->shared_from_this();  // the receiver may destroy the operation
            stopCallback.reset();
            switch (outcome) {
                case Outcome::value:
                    ex::set_value(std::move(receiver));
                    break;
                case Outcome::stopped:
                    ex::set_stopped(std::move(receiver));
                    break;
                case Outcome::error:
                    ex::set_error(std::move(receiver), std::move(failure));
                    break;
                case Outcome::pending:
                    std::terminate();
            }
        }
    };

    template <typename Receiver>
    class Operation {
        std::shared_ptr<State<Receiver>> state;

    public:
        using operation_state_concept = ex::operation_state_tag;
        explicit Operation(std::shared_ptr<State<Receiver>> value) : state(std::move(value)) { }
        Operation(const Operation&)            = delete;
        Operation(Operation&&)                 = delete;
        Operation& operator=(const Operation&) = delete;
        Operation& operator=(Operation&&)      = delete;
        void       start() noexcept {
            auto keepAlive = state;
            keepAlive->start();
        }
    };

public:
    using sender_concept        = ex::sender_tag;
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;

    explicit UIScheduleSender(std::weak_ptr<UIExecutorShared> value) : target(std::move(value)) { }

    template <typename Receiver>
    auto connect(Receiver receiver) const {
        return Operation<Receiver>{std::make_shared<State<Receiver>>(std::move(receiver), target)};
    }
};

}  // namespace detail

/// @brief Handle to the interface thread of a frontend. Cheap to copy; a default-constructed one is never available.
class UIExecutor {
    std::weak_ptr<detail::UIExecutorShared> shared;

public:
    UIExecutor() = default;
    explicit UIExecutor(std::weak_ptr<detail::UIExecutorShared> value) : shared(std::move(value)) { }

    /// True while the frontend's loop runs. A later call to schedule() can still find it closed.
    [[nodiscard]] bool available() const {
        const auto state = shared.lock();
        return state && state->running();
    }
    /// True when called from the interface thread of a running loop.
    [[nodiscard]] bool onUIThread() const {
        const auto state = shared.lock();
        return state && state->onUIThread();
    }
    /// A sender completing on the interface thread; see the file comment for the other completions. Compose it with
    /// `ex::then` to run a function there. The operation always goes through the queue, even from the interface thread.
    [[nodiscard]] auto schedule() const {
        return detail::UIScheduleSender{shared};
    }
};

/// @brief The frontend's side of the executor: owns the queue and is driven by the interface thread's loop.
class UIExecutorHost {
    std::shared_ptr<detail::UIExecutorShared> shared{std::make_shared<detail::UIExecutorShared>()};

public:
    [[nodiscard]] UIExecutor executor() const {
        return UIExecutor{shared};
    }
    /// Declares the calling thread as the interface thread. `wake` is thread-safe and makes the loop call drain() soon.
    /// Throws std::logic_error when a loop is already attached; after close() a new loop may attach (handles stay valid).
    void attach(std::function<void()> wake) {
        shared->attach(std::move(wake));
    }
    /// Runs a bounded batch of the queued operations (those queued when it starts) and wakes the loop again if more
    /// remain; call it on the interface thread only.
    void drain() noexcept {
        shared->drain();
    }
    /// Ends the loop: pending operations complete stopped, later ones fail as unavailable. Idempotent. Returns once the
    /// wake-ups in flight on other threads are over, so the loop may then free what `wake` uses (never call it from `wake`).
    void close() noexcept {
        shared->close();
    }
};

/// @brief The process-wide host of one frontend type (`Tag` is the frontend's own type): its adapter attaches it while its loop runs.
template <typename Tag>
[[nodiscard]] UIExecutorHost& uiExecutorHostOf() {
    static UIExecutorHost host;
    return host;
}

}  // namespace webfront::frontend
