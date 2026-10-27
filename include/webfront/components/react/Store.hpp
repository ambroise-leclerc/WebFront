/// @brief Optional per-browser React snapshots, coalesced on the server context.
#pragma once
#include "../../dom/Document.hpp"
#include "../../tooling/Logger.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace webfront::react {
/** @brief Last snapshot wins within a 1 ms cycle. T must be a bridge value (a tuple for several fields).
 * Construct after the page has installed its named store. Copies share pending state.
 * All access follows the DOM document's server-thread and connection lifetime rules.
 */
template <typename UI, typename T>
class Store {
    struct State {
        UI                                      ui;
        dom::Document<UI>                       document;
        std::string                             function;
        std::optional<T>                        pending;
        bool                                    scheduled{}, closed{};
        std::function<void(std::exception_ptr)> onError;

        bool valid() const noexcept {
            return document.valid() && !closed;
        }
        void check() const {
            if (!valid())
                throw std::runtime_error("React store is closed or used off the server thread");
        }
        void flush() {
            check();
            if (!pending)
                return;
            ui.jsFunction(function)(*pending);
            pending.reset();
        }
    };
    std::shared_ptr<State> state;

    void check() const {
        if (!state)
            throw std::runtime_error("React store was moved from");
        state->check();
    }

    static Task sendAfter(std::shared_ptr<State> state) {
        co_await state->ui.resumeAfter(std::chrono::milliseconds(1));
        state->scheduled = false;
        if (!state->valid())
            co_return;
        try {
            state->flush();
        } catch (...) {
            state->closed = true;
            state->pending.reset();
            auto callback = std::exchange(state->onError, nullptr);
            try {
                if (callback)
                    callback(std::current_exception());
            } catch (...) {
                try {
                    log::error("React store onError callback threw");
                } catch (...) {
                    // A custom log sink must not let the user's error escape the coroutine either.
                }
            }
        }
    }

public:
    Store(UI ui, std::string name) {
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            throw std::invalid_argument("Invalid React store name");
        state = std::make_shared<State>(State{ui, ui.document(), "webfrontReactStores." + name, {}, false, false, {}});
    }
    bool valid() const noexcept {
        return state && state->valid();
    }
    void set(T value) const {
        check();
        state->pending = std::move(value);
        if (!state->scheduled) {
            state->scheduled = true;
            try {
                static_cast<void>(state->ui.spawn(sendAfter(state)));
            } catch (...) {
                state->scheduled = false;
                throw;
            }
        }
    }
    /// Queues the last snapshot immediately; it does not acknowledge browser rendering.
    void flush() const {
        check();
        state->flush();
    }
    void onError(std::function<void(std::exception_ptr)> callback) const {
        check();
        state->onError = std::move(callback);
    }
    void close() const {
        check();
        state->closed = true;
        state->pending.reset();
        state->onError = nullptr;
    }
};
}  // namespace webfront::react
