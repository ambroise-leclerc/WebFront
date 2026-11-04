/// @brief Server-owned thread running blocking application work (route handlers, file reads) off the I/O context.
#pragma once
#include "../networking/Tasks.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::http {

/// Runs the work senders it hands out on lazily started threads, up to a fixed number: a job waits for a free
/// thread, so slow work delays only the jobs queued behind as many slow ones as there are threads.
///
/// A sender completes on that thread: the awaiting task continues on the I/O context with `continues_on`.
/// Work still queued when the worker stops completes as stopped, so no task outlives the shutdown; the work
/// already running is not interrupted. A stop request of the awaiting task skips work not yet started.
class BlockingWorker {
    struct Job {
        virtual void run() noexcept    = 0;
        virtual void cancel() noexcept = 0;

    protected:
        ~Job() = default;
    };

public:
    explicit BlockingWorker(std::size_t maxThreads = 1) : limit(maxThreads == 0 ? 1 : maxThreads) {}
    BlockingWorker(const BlockingWorker&)            = delete;
    BlockingWorker& operator=(const BlockingWorker&) = delete;
    ~BlockingWorker() {
        stop();
        join();
    }

    /// Thread-safe. Refuses new work and lets the thread end once the queued work is cancelled.
    void stop() noexcept {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
    }

    /// Waits for the running work; call after `stop()` and outside the worker's own thread.
    void join() noexcept {
        std::vector<std::thread> ending;
        {
            std::lock_guard lock(mutex);
            ending.swap(threads);
        }
        for (auto& thread : ending)
            if (thread.joinable()) thread.join();
    }

    struct Sender {
        using sender_concept        = networking::ex::sender_t;
        using completion_signatures = networking::ex::
            completion_signatures<networking::ex::set_value_t(), networking::ex::set_error_t(std::exception_ptr), networking::ex::set_stopped_t()>;

        BlockingWorker*       worker;
        std::function<void()> work;

        template <typename Receiver>
        struct Operation final : Job {
            BlockingWorker*       worker;
            Receiver              receiver;
            std::function<void()> work;
            Operation(BlockingWorker* owner, Receiver value, std::function<void()> function)
                : worker(owner), receiver(std::move(value)), work(std::move(function)) {}
            Operation(const Operation&) = delete;
            Operation(Operation&&)      = delete;

            void start() noexcept {
                try {
                    if (worker->enqueue(this)) return;
                } catch (...) {
                    networking::ex::set_error(std::move(receiver), std::current_exception());
                    return;
                }
                networking::ex::set_stopped(std::move(receiver));
            }
            // Completing may destroy this operation: nothing touches a member afterwards.
            void run() noexcept override {
                if (networking::ex::get_stop_token(networking::ex::get_env(receiver)).stop_requested()) {
                    networking::ex::set_stopped(std::move(receiver));
                    return;
                }
                try {
                    work();
                } catch (...) {
                    networking::ex::set_error(std::move(receiver), std::current_exception());
                    return;
                }
                networking::ex::set_value(std::move(receiver));
            }
            void cancel() noexcept override { networking::ex::set_stopped(std::move(receiver)); }
        };

        template <typename Receiver>
        auto connect(Receiver receiver) && {
            return Operation<Receiver>(worker, std::move(receiver), std::move(work));
        }
    };

    /// A sender running `work` on the worker thread. State it touches must outlive the await.
    [[nodiscard]] Sender run(std::function<void()> work) { return {this, std::move(work)}; }

private:
    std::mutex              mutex;
    std::condition_variable changed;
    std::deque<Job*>        queue;
    bool                    stopping = false;
    std::vector<std::thread> threads;
    std::size_t             limit;
    std::size_t             idle = 0;

    bool enqueue(Job* job) {
        {
            std::lock_guard lock(mutex);
            if (stopping) return false;
            queue.push_back(job);
            if (queue.size() > idle && threads.size() < limit) {
                try {
                    threads.emplace_back([this] { loop(); });
                } catch (...) {
                    if (threads.empty()) {
                        queue.pop_back();
                        throw;
                    }  // else a running thread will take the job
                }
            }
        }
        changed.notify_one();
        return true;
    }

    void loop() noexcept {
        for (;;) {
            Job* job = nullptr;
            std::deque<Job*> cancelled;
            {
                std::unique_lock lock(mutex);
                ++idle;
                changed.wait(lock, [this] { return stopping || !queue.empty(); });
                --idle;
                if (stopping) {
                    cancelled.swap(queue);
                } else {
                    job = queue.front();
                    queue.pop_front();
                }
            }
            if (job) {
                job->run();
                continue;
            }
            for (auto* pending : cancelled) pending->cancel();
            return;
        }
    }
};

} // namespace webfront::http
