/// @brief Lazy, cancellable C++ to browser calls using C++26 sender completions.
#pragma once
#include "http/WebSocket.hpp"
#include "weblink/PendingCalls.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace webfront::detail {
namespace ex = networking::ex;

template <typename R>
struct CallCompletions {
    using type = ex::completion_signatures<ex::set_value_t(R), ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;
};
template <>
struct CallCompletions<void> {
    using type = ex::completion_signatures<ex::set_value_t(), ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;
};

template <typename Net, http::BuffersPolicyType Policy, typename R>
class BrowserCallSender {
    using Registry = PendingCalls<Policy>;
    using Socket   = websocket::WebSocket<Net, Policy>;
    std::weak_ptr<Registry> pending;
    std::weak_ptr<Socket>   socket;
    std::vector<std::byte>  payload;

    template <typename Receiver>
    struct State : std::enable_shared_from_this<State<Receiver>> {
        using Token = ex::stop_token_of_t<ex::env_of_t<Receiver>>;
        struct OnStop {
            std::weak_ptr<State> state;
            void                 operator()() noexcept {
                if (auto shared = state.lock())
                    shared->finish(Outcome::stopped);
            }
        };
        enum class Outcome { pending, value, error, stopped };
        Receiver                                                                receiver;
        std::weak_ptr<Registry>                                                 pending;
        std::weak_ptr<Socket>                                                   socket;
        std::vector<std::byte>                                                  payload;
        std::mutex                                                              mutex;
        bool                                                                    starting{true};
        Outcome                                                                 outcome{Outcome::pending};
        std::optional<std::conditional_t<std::is_void_v<R>, std::monostate, R>> value;
        std::exception_ptr                                                      error;
        msg::CallId                                                             id{};
        std::optional<ex::stop_callback_for_t<Token, OnStop>>                   stopCallback;

        State(Receiver r, std::weak_ptr<Registry> calls, std::weak_ptr<Socket> ws, std::vector<std::byte> data)
            : receiver(std::move(r)), pending(std::move(calls)), socket(std::move(ws)), payload(std::move(data)) { }

        void start() noexcept {
            // Completion is deferred until callback construction and dispatch are finished:
            // registering a previously stopped token may invoke OnStop inline.
            try {
                stopCallback.emplace(ex::get_stop_token(ex::get_env(receiver)), OnStop{this->weak_from_this()});
                registerCall();
                dispatch();
            } catch (...) {
                finish(Outcome::error, std::current_exception());
            }
            bool ready{};
            {
                std::lock_guard lock(mutex);
                starting = false;
                ready    = outcome != Outcome::pending;
            }
            if (ready)
                deliver();
        }

        void registerCall() {
            std::lock_guard lock(mutex);
            if (outcome != Outcome::pending)
                return;
            auto calls = pending.lock();
            if (!calls)
                throw std::runtime_error("Browser connection closed");
            auto self = this->shared_from_this();
            id        = calls->add({.complete =
                                        [self](const msg::FunctionReturn<Policy>& result) {
                                     self->complete(result);
                                        },
                                    .reject =
                                        [self](std::exception_ptr e) {
                                     self->finish(Outcome::error, std::move(e));
                                        },
                                    .owner = this});
        }

        void dispatch() {
            {
                std::lock_guard lock(mutex);
                if (outcome != Outcome::pending)
                    return;
            }
            if (id == 0)
                return;
            std::memcpy(payload.data() + offsetof(msg::FunctionCallHeader<msg::Command::callFunction>, callId), &id, sizeof(id));
            auto ws = socket.lock();
            if (!ws)
                throw std::runtime_error("Browser connection closed");
            ws->write(websocket::Frame<Net>{std::span<const std::byte>(payload)});
        }

        void complete(const msg::FunctionReturn<Policy>& result) noexcept {
            try {
                if constexpr (std::is_void_v<R>) {
                    decodeReturn<void>(result);
                    finish(Outcome::value);
                } else {
                    auto decoded = decodeReturn<R>(result);
                    bool ready{};
                    {
                        std::lock_guard lock(mutex);
                        if (outcome != Outcome::pending)
                            return;
                        value.emplace(std::move(decoded));
                        outcome = Outcome::value;
                        ready   = !starting;
                    }
                    if (ready)
                        deliver();
                }
            } catch (...) {
                finish(Outcome::error, std::current_exception());
            }
        }

        void finish(Outcome completion, std::exception_ptr failure = {}) noexcept {
            bool ready{};
            {
                std::lock_guard lock(mutex);
                if (outcome != Outcome::pending)
                    return;
                outcome = completion;
                error   = std::move(failure);
                ready   = !starting;
            }
            if (ready)
                deliver();
        }

        void discardPending() noexcept {
            if (auto calls = pending.lock())
                calls->cancel(id, this);
        }

        void deliverValue() noexcept {
            if constexpr (std::is_void_v<R>)
                ex::set_value(std::move(receiver));
            else
                ex::set_value(std::move(receiver), std::move(*value));
        }

        void deliver() noexcept {
            // Exactly one completion won under mutex. Unregister outside that mutex:
            // another thread's stop callback may be waiting to inspect the winner.
            if (outcome != Outcome::value)
                discardPending();
            stopCallback.reset();
            switch (outcome) {
                case Outcome::value:
                    deliverValue();
                    break;
                case Outcome::error:
                    ex::set_error(std::move(receiver), std::move(error));
                    break;
                case Outcome::stopped:
                    ex::set_stopped(std::move(receiver));
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
        Operation(const Operation&)                                  = delete;
        Operation(Operation&&)                                       = delete;
        Operation&                       operator=(const Operation&) = delete;
        Operation&                       operator=(Operation&&)      = delete;
        void                             start() noexcept {
            auto keepAlive = state;  // receiver completion may destroy this operation inline
            keepAlive->start();
        }
    };

public:
    using sender_concept        = ex::sender_tag;
    using completion_signatures = typename CallCompletions<R>::type;

    BrowserCallSender(std::weak_ptr<Registry> calls, std::weak_ptr<Socket> ws, std::vector<std::byte> data)
        : pending(std::move(calls)), socket(std::move(ws)), payload(std::move(data)) { }

    template <typename Receiver>
    auto connect(Receiver receiver) && {
        return Operation<Receiver>{std::make_shared<State<Receiver>>(std::move(receiver), pending, socket, std::move(payload))};
    }
    template <typename Receiver>
    auto connect(Receiver receiver) const& {
        return Operation<Receiver>{std::make_shared<State<Receiver>>(std::move(receiver), pending, socket, payload)};
    }
};
}  // namespace webfront::detail

namespace webfront {
/** @brief Encodes owned arguments now; each connected sender dispatches only when started. */
template <typename WebFront, typename R = void>
class JsFunctionSender {
    using Net    = typename WebFront::Net;
    using Policy = typename WebFront::BufferPolicy;
    std::string                                      name;
    std::weak_ptr<detail::PendingCalls<Policy>>      pending;
    std::weak_ptr<websocket::WebSocket<Net, Policy>> socket;

public:
    JsFunctionSender(std::string_view functionName, WebFront& wf, WebLinkId id) : name(functionName) {
        auto& link = wf.getLink(id);
        pending    = link.pendingResults();
        socket     = link.socket();
    }

    auto operator()(auto&&... arguments) const {
        msg::FunctionCall<Policy> command;
        websocket::Frame<Net>     frame{std::span(reinterpret_cast<const std::byte*>(command.header().data()), command.header().size())};
        command.encodeParameter(name, frame);
        (command.encodeParameter(std::forward<decltype(arguments)>(arguments), frame), ...);
        command.setParametersCount(static_cast<std::uint8_t>(1 + sizeof...(arguments)));
        std::vector<std::byte> payload;
        payload.reserve(frame.size());
        auto buffers = frame.toBuffers();
        for (std::size_t i = 1; i < buffers.size(); ++i) {
            const auto* data = static_cast<const std::byte*>(buffers[i].data());
            payload.insert(payload.end(), data, data + buffers[i].size());
        }
        return detail::BrowserCallSender<Net, Policy, R>{pending, socket, std::move(payload)};
    }
};
}  // namespace webfront
