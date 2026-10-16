/// @brief Coroutine helpers shared by the HTTP server and the WebSocket.
#pragma once

#include "../tooling/Logger.hpp"
#include "SenderHeaders.hpp"

#include <exception>

namespace webfront::networking {

namespace ex = ::stdnet::ex;

/// A coroutine awaiting network senders. Errors of the awaited operations are thrown as std::system_error.
using Task = ex::task<void>;

/// Runs `task` on `scheduler` until it ends or `stop` is requested; the task owns what it needs. A stop
/// request, possible from any thread, cancels the operation the task awaits and unwinds it on the scheduler.
template <typename Scheduler>
void Spawn(Scheduler scheduler, ex::inplace_stop_token stop, Task task) {
    auto logError = [](std::exception_ptr error) noexcept {
        try {
            std::rethrow_exception(error);
        }
        catch (const std::exception& e) {
            log::error("Network task failed: {}", e.what());
        }
        catch (...) {
            log::error("Network task failed");
        }
    };
    exec::start_detached(ex::upon_error(ex::starts_on(scheduler, ex::write_env(std::move(task), ex::prop{ex::get_stop_token, stop})), logError));
}

}  // namespace webfront::networking
