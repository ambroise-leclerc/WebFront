/// @brief Coroutine helpers and structured ownership shared by HTTP and WebSocket tasks.
#pragma once

#include "../tooling/Logger.hpp"
#include "SenderHeaders.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace webfront::networking {

// The single implementation alias: C++26 P2300 senders/receivers, task, and
// P3149 counting_scope/spawn/join. stdnet selects stdexec until the STL supplies them.
namespace ex = ::stdnet::ex;

/// A coroutine awaiting network senders. I/O errors are thrown as std::system_error.
using Task = ex::task<void>;

/** @brief Owns network tasks until an explicit join, independently of unrelated context work. */
class TaskScope {
    struct Joined {
        using receiver_concept = ex::receiver_tag;
        std::atomic_bool* complete;
        void              set_value() && noexcept {
            complete->store(true, std::memory_order_release);
        }
    };

    struct SpawnToken {
        decltype(std::declval<ex::counting_scope&>().get_token()) token;
        bool*                                                     associated;
        auto                                                      try_associate() const noexcept {
            auto association = token.try_associate();
            *associated      = static_cast<bool>(association);
            return association;
        }
        template <ex::sender Sender>
        auto wrap(Sender&& sender) const noexcept(noexcept(token.wrap(std::forward<Sender>(sender)))) {
            return token.wrap(std::forward<Sender>(sender));
        }
    };

    ex::counting_scope tasks;
    using JoinOperation = ex::connect_result_t<decltype(ex::starts_on(ex::inline_scheduler{}, std::declval<ex::counting_scope&>().join())), Joined>;
    std::atomic_bool               joined{false};
    std::unique_ptr<JoinOperation> joining;
    // A connection has its own stop source. This token links it to server shutdown
    // using standard stop callbacks, without relying on an implementation's fused tokens.
    ex::inplace_stop_source stopSource;

public:
    /** @brief Prevents any further task from associating with this scope. */
    void close() noexcept {
        tasks.close();
    }

    template <ex::sender Sender>
    bool spawn(Sender sender) {
        if (stopRequested())
            return false;
        // spawn associates synchronously. Observe that decision so a close
        // racing this call cannot leave the caller with an apparently live handle.
        bool associated = false;
        ex::spawn(std::move(sender), SpawnToken{tasks.get_token(), &associated});
        return associated;
    }
    /** @brief Cancels all associated tasks and each connection linked to this scope. */
    void requestStop() noexcept {
        stopSource.request_stop();
        tasks.request_stop();
    }
    bool stopRequested() const noexcept {
        return stopSource.stop_requested();
    }
    auto getToken() noexcept {
        return tasks.get_token();
    }
    auto stopToken() noexcept {
        return stopSource.get_token();
    }

    /** @brief Closes, stops and joins while pumping only as long as tracked tasks remain.
     * Call on the context's owning thread, after its other run() callers have returned.
     */
    template <typename Context>
    void drain(Context& context) {
        close();
        requestStop();
        context.restart();
        if (!joining) {
            joining.reset(new JoinOperation(ex::connect(ex::starts_on(ex::inline_scheduler{}, tasks.join()), Joined{&joined})));
            ex::start(*joining);
        }
        while (!joined.load(std::memory_order_acquire)) {
            if (context.run_one() == 0 && !joined.load(std::memory_order_acquire))
                throw std::logic_error("Task scope cannot join: context has no work but tasks remain suspended");
        }
    }

    /** @brief Destructor path: never abandon live tasks or let a drain error escape.
     * A non-cooperative external operation can delay destruction until it completes.
     */
    template <typename Context>
    void drainUntilJoined(Context& context) noexcept {
        bool reported = false;
        for (;;) {
            try {
                drain(context);
                return;
            } catch (...) {
                if (!reported) {
                    reportDeferredShutdown();
                    reported = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

private:
    static void reportDeferredShutdown() noexcept {
        try {
            log::error("Task shutdown has not joined yet; waiting for suspended operations to finish");
        } catch (...) { }  // Logging must not make the destructor path throw.
    }
};

/// Logs a network task's unhandled error without changing its stopped completion.
inline void logTaskError(std::exception_ptr error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        log::error("Network task failed: {}", e.what());
    } catch (...) {
        log::error("Network task failed");
    }
}

/** @brief Starts a task on its scheduler, tracked until its entire operation state is destroyed. */
template <typename Scheduler, ex::sender Sender>
bool Spawn(TaskScope& scope, Scheduler scheduler, Sender task) {
    return scope.spawn(ex::upon_error(ex::starts_on(scheduler, std::move(task)), logTaskError));
}

/** @brief Starts a tracked task with a connection's stop token, linked to the scope by its owner. */
template <typename Scheduler>
bool Spawn(TaskScope& scope, Scheduler scheduler, ex::inplace_stop_token stop, Task task) {
    return Spawn(scope, scheduler, ex::write_env(std::move(task), ex::prop{ex::get_stop_token, stop}));
}

}  // namespace webfront::networking
