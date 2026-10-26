/// @brief Subscriptions to connecting and disconnecting browsers (BasicWF::subscribeUIStarted, subscribeUIClosed).
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace webfront {

/// Keeps a BasicWF::subscribeUIStarted or subscribeUIClosed handler registered while it lives. Destroying or resetting it
/// unsubscribes; this is safe after the application itself was destroyed. Move-only.
class UISubscription {
public:
    UISubscription() = default;
    explicit UISubscription(std::function<void()> unsubscribe) : cancel(std::move(unsubscribe)) {}
    UISubscription(UISubscription&& other) noexcept : cancel(std::exchange(other.cancel, {})) {}
    UISubscription& operator=(UISubscription&& other) noexcept {
        if (this != &other) {
            reset();
            cancel = std::exchange(other.cancel, {});
        }
        return *this;
    }
    UISubscription(const UISubscription&)            = delete;
    UISubscription& operator=(const UISubscription&) = delete;
    ~UISubscription() {
        reset();
    }

    /// Unsubscribes now. Takes effect for browsers that connect (or disconnect) afterwards.
    void reset() {
        if (auto unsubscribe = std::exchange(cancel, {}))
            unsubscribe();
    }

    [[nodiscard]] bool active() const {
        return static_cast<bool>(cancel);
    }

private:
    std::function<void()> cancel;
};

namespace detail {

/// Handlers notified, in subscription order, of each connected (or disconnected) browser. Thread-safe:
/// browsers connect on the server thread while handlers may be added or removed from any thread.
template <typename UI>
class UISubscribers : public std::enable_shared_from_this<UISubscribers<UI>> {
public:
    using Handler = std::function<void(UI)>;

    UISubscription subscribe(Handler handler) {
        std::scoped_lock lock{mutex};
        const auto       id = nextId++;
        handlers.emplace_back(id, std::make_shared<Handler>(std::move(handler)));
        return UISubscription{[weakSelf = this->weak_from_this(), id] {
            if (auto self = weakSelf.lock())
                self->unsubscribe(id);
        }};
    }

    /// Calls every handler subscribed when the notification starts, outside the lock, so a handler
    /// may subscribe or unsubscribe without deadlocking.
    void notify(const UI& ui) const {
        for (const auto& handler : snapshot())
            (*handler)(ui);
    }

    [[nodiscard]] std::size_t size() const {
        std::scoped_lock lock{mutex};
        return handlers.size();
    }

private:
    mutable std::mutex                                              mutex;
    std::uint64_t                                                   nextId = 0;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<Handler>>> handlers;

    // The removed handler is destroyed after the lock is released: its captures may own another
    // subscription to these subscribers, whose destruction unsubscribes and takes the lock again.
    void unsubscribe(std::uint64_t id) {
        std::shared_ptr<Handler> removed;
        {
            std::scoped_lock lock{mutex};
            auto             found = std::ranges::find(handlers, id, [](const auto& entry) {
                return entry.first;
            });
            if (found == handlers.end())
                return;
            removed = std::move(found->second);
            handlers.erase(found);
        }
    }

    [[nodiscard]] std::vector<std::shared_ptr<Handler>> snapshot() const {
        std::scoped_lock                      lock{mutex};
        std::vector<std::shared_ptr<Handler>> copy;
        copy.reserve(handlers.size());
        for (const auto& entry : handlers)
            copy.push_back(entry.second);
        return copy;
    }
};

}  // namespace detail
}  // namespace webfront
