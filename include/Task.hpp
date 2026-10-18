/// @brief Application coroutines owned by WebFront, with optional individual cancellation.
#pragma once
#include "networking/Tasks.hpp"

#include <memory>
#include <utility>

namespace webfront {
using Task = networking::Task;

/** @brief Requests cancellation of one spawned task. Discarding the handle does not stop it. */
class TaskHandle {
    std::shared_ptr<networking::ex::inplace_stop_source> source{std::make_shared<networking::ex::inplace_stop_source>()};

public:
    void requestStop() const noexcept {
        auto keepAlive = source;
        keepAlive->request_stop();
    }
    bool stopRequested() const noexcept {
        return source->stop_requested();
    }
    auto stopToken() const noexcept {
        return source->get_token();
    }
};

namespace detail {
struct StopTask {
    TaskHandle handle;
    void       operator()() noexcept {
        handle.requestStop();
    }
};

struct TaskRegistration {
    TaskHandle                                                                        handle;
    networking::ex::stop_callback_for_t<networking::ex::inplace_stop_token, StopTask> stop;
    TaskRegistration(TaskHandle value, networking::ex::inplace_stop_token parent) : handle(std::move(value)), stop(parent, StopTask{handle}) { }
};

inline Task ownedTask(Task task, std::unique_ptr<TaskRegistration> registration) {
    co_await networking::ex::write_env(std::move(task), networking::ex::prop{networking::ex::get_stop_token, registration->handle.stopToken()});
}
}  // namespace detail
}  // namespace webfront
