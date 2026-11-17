#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>

namespace webfront::testdriver {
// The native watchdog keeps both the budget of the whole invocation and the limit of the command, including when a spec
// blocks JavaScript so that no JS timer can report an expired command.
class DriverDeadline {
public:
    explicit DriverDeadline(std::chrono::steady_clock::duration total) : overall(std::chrono::steady_clock::now() + total), current(overall) {}
    void beginCase(std::chrono::steady_clock::duration timeout) {
        std::scoped_lock lock{mutex};
        current = std::min(overall, std::chrono::steady_clock::now() + timeout);
        ++revision;
        changed.notify_all();
    }
    void endCase() {
        std::scoped_lock lock{mutex};
        current = overall;
        ++revision;
        changed.notify_all();
    }
    // The page is done: expired() returns false, as when it is stopped.
    void finish() {
        std::scoped_lock lock{mutex};
        finished = true;
        ++revision;
        changed.notify_all();
    }
    // True when a limit elapsed; false when stopped or finished first.
    bool expired(std::stop_token stop) {
        std::unique_lock lock{mutex};
        while (!stop.stop_requested() && !finished) {
            const auto previous = revision;
            const auto limit    = current;
            if (!changed.wait_until(lock, stop, limit, [&] {
                    return previous != revision;
                }))
                return !stop.stop_requested();
        }
        return false;
    }

private:
    const std::chrono::steady_clock::time_point overall;
    std::chrono::steady_clock::time_point       current;
    std::size_t                                 revision = 0;
    bool                                        finished = false;
    std::mutex                                  mutex;
    std::condition_variable_any                 changed;
};
}  // namespace webfront::testdriver
