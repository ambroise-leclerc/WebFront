// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-28
//
// WebFront — core
// Role: The reads of the inspectors for a caller that is no page (ADR-027, section 10): the same pages, in the same bounds, without the write.
// Thread: thread-safe | Alloc: unrestricted
// LLM: given by DeveloperApp::inspectorReads() to automation's MCP tools; never call wait() from the server thread

#pragma once

#include "Introspection.hpp"
#include "Task.hpp"
#include "Watches.hpp"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>

namespace webfront::inspect {

namespace detail {
/// Runs the operation `make` builds and gives its answer, or its exception, to `out`. A task stopped before its end
/// (the application shuts down) destroys `out`, which the waiting thread sees as a broken promise.
template <typename Result>
Task deliver(std::function<ResultTask<Result>()> make, std::shared_ptr<std::promise<Result>> out) {
    try {
        out->set_value(co_await make());
    } catch (...) {
        out->set_exception(std::current_exception());
    }
}
}  // namespace detail

/**
 * @brief What the inspectors read, for native code that is no page of the application: the functions, the calls to
 * JavaScript, the browsers and tasks, and the watches (their discovery, their trees, their differences, a whole value).
 *
 * The answers are those of the `webfront.inspector.*` functions of the same names, computed by the same code with the same
 * bounds (ADR-027, sections 3 and 7); the write of section 6 is not here. The watches are read with a memory of their own,
 * the one a connection would have: `fold` and `reset` change only that memory, never the application nor what a panel shows.
 *
 * The four first reads answer on the calling thread. The reads of a watch are operations that run where a page's would:
 * on the server thread, then on the watch's executor when it has one. wait() runs one from another thread and blocks until
 * its answer; the operations asked through it run in the order they are asked.
 */
struct Reads {
    Limits                                                                limits;
    std::function<FunctionPage(double after, double count)>               functions;
    std::function<JsFunctionPage(double after, double count)>             jsFunctions;
    std::function<BrowserPage(double from, double count)>                 browsers;
    std::function<WatchPage(double revision, double after, double count)> watches;

    std::function<ResultTask<Page>(double watch, std::string path, double revision, double start, double count)> children;
    std::function<ResultTask<Page>(double watch, double revision, double cursor, double count)>                  changes;
    std::function<ResultTask<ValueAnswer>(double watch, std::string path, double snapshot, double offset)>       value;
    std::function<ResultTask<Page>(double watch, std::string path, double revision)>                             fold;
    std::function<ResultTask<double>(double watch)>                                                              reset;

    /// Starts a task on the server thread (BasicWF::spawn).
    std::function<TaskHandle(Task)> spawn;

    /**
     * Runs `make()` on the server thread and returns its answer, or throws what it threw. Throws `webfront.inspector.unavailable`
     * when the application does not run it within `patience` (its server is not running, or is shutting down); the operation
     * is then asked to stop, and one that had started changes only the memory of these reads.
     * Never call it from the server thread: it would wait for itself.
     */
    template <typename Result>
    [[nodiscard]] Result wait(std::function<ResultTask<Result>()> make, std::chrono::milliseconds patience = std::chrono::seconds{10}) const {
        auto       promise = std::make_shared<std::promise<Result>>();
        auto       future  = promise->get_future();
        const auto handle  = spawn(detail::deliver<Result>(std::move(make), std::move(promise)));
        if (handle.stopRequested())
            throw inspect::detail::refusal(code::unavailable, "The application is shutting down");
        if (future.wait_for(patience) != std::future_status::ready) {
            handle.requestStop();
            throw inspect::detail::refusal(code::unavailable, "The application did not run the read in time: its server is not running, or is busy");
        }
        try {
            return future.get();
        } catch (const std::future_error&) {
            throw inspect::detail::refusal(code::unavailable, "The application stopped before the read ended");
        }
    }
};

}  // namespace webfront::inspect
