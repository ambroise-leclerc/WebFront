/// @brief The JavaScript calls a C++ callback has not answered yet, per browser connection.
#pragma once

#include "Task.hpp"
#include "weblink/Messages.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::detail {

template <class... Values>
struct single_value {
    static_assert(sizeof...(Values) <= 1, "A deferred callback must complete with at most one value");
    using type = void;
};
template <class Value>
struct single_value<Value> {
    using type = std::decay_t<Value>;
};
template <class... Alternatives>
struct only_completion {
    static_assert(sizeof...(Alternatives) == 1, "A deferred callback must have exactly one kind of value completion");
};
template <class Alternative>
struct only_completion<Alternative> {
    using type = typename Alternative::type;
};

/// A callback answers later when it returns a sender (a `ResultTask<R>` among them) instead of the result itself.
template <typename Result>
concept deferred_result = !std::is_void_v<Result> && networking::ex::sender<std::remove_cvref_t<Result>>;

/// The value the sender completes with (`void` for none).
template <typename Sender>
using deferred_value_t = typename networking::ex::value_types_of_t<std::remove_cvref_t<Sender>, networking::ex::env<>, single_value, only_completion>;

/// The calls of one connection that a callback answers later, with the tasks that will answer them. The connection's
/// closure stops every one of them, and a call whose identifier is still in flight cannot be started again.
class DeferredCalls {
public:
    /// Claims the identifier of a call (zero, which asks for no answer, is never in flight) and returns the key of its
    /// entry, or 0 when the connection closed or a call of that identifier has not been answered yet.
    [[nodiscard]] std::uint64_t reserve(msg::CallId callId) {
        std::lock_guard lock(mutex);
        if (closed || (callId != 0 && !inFlight.insert(callId).second))
            return 0;
        const auto key = ++lastKey;
        tasks.emplace(key, std::optional<TaskHandle>{});
        return key;
    }

    /// Remembers the task that will answer the reserved call, to stop it if the connection closes.
    void attach(std::uint64_t key, TaskHandle handle) {
        bool stop = false;
        {
            std::lock_guard lock(mutex);
            if (auto found = tasks.find(key); found != tasks.end())
                found->second = handle;
            stop = closed;
        }
        if (stop)
            handle.requestStop();
    }

    /// The call is answered, or dropped: its identifier can be used again.
    void finish(msg::CallId callId, std::uint64_t key) {
        std::lock_guard lock(mutex);
        inFlight.erase(callId);
        tasks.erase(key);
    }

    /// The connection closed: every unanswered call is stopped and its late answer will be dropped.
    void close() {
        std::vector<TaskHandle> stopping;
        {
            std::lock_guard lock(mutex);
            closed = true;
            for (const auto& [key, handle] : tasks)
                if (handle)
                    stopping.push_back(*handle);
        }
        for (const auto& handle : stopping)
            handle.requestStop();
    }

    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock(mutex);
        return tasks.size();
    }

private:
    mutable std::mutex                 mutex;
    std::set<msg::CallId>              inFlight;
    std::map<std::uint64_t, std::optional<TaskHandle>> tasks;
    std::uint64_t                      lastKey = 0;
    bool                               closed  = false;
};

}  // namespace webfront::detail
