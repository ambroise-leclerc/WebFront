// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief A JavaScript function called on every connected browser (BasicWF::broadcast).
#pragma once
#include "http/WebSocket.hpp"
#include "weblink/Messages.hpp"
#include "weblink/WebLink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront {

/// How a broadcast treats a browser that reads slower than it is written.
struct BroadcastOptions {
    /// Without a value, every call is queued for every browser. With one, a call is queued for a browser only while
    /// fewer frames than this await its network; otherwise it replaces the call held back for that browser, queued as
    /// soon as fewer frames await: a slow browser skips intermediate calls but always receives the latest. Must be > 0.
    std::optional<std::size_t> maxPendingWrites{};
};

/// Calls a JavaScript function on each browser connected at the time of the call, fire-and-forget, from any thread.
/// The arguments are encoded once. Copies share the calls held back for slow browsers; the object must not outlive
/// its application.
template <typename WebFront>
class Broadcast {
    using Net     = typename WebFront::Net;
    using Link    = WebLink<Net, typename WebFront::BufferPolicy>;
    using Message = std::shared_ptr<const std::vector<std::byte>>;

    // The call held back for one browser; guarded by State::mutex. Holds no strong reference to its link, so that
    // no link can be destroyed, and wake its write waiters, under that mutex.
    struct Slot {
        std::weak_ptr<Link> link;
        Message             held;
        bool                sending = false;  // a thread or a write waiter is queuing 'held'
    };
    struct State {
        std::mutex                                 mutex;
        std::map<WebLinkId, std::shared_ptr<Slot>> slots;
    };

public:
    Broadcast(std::string_view functionName, WebFront& wf, BroadcastOptions options = {})
        : name(functionName), webFront(wf), maxPending(options.maxPendingWrites) {
        if (maxPending && *maxPending == 0)
            throw std::invalid_argument("BroadcastOptions::maxPendingWrites must be at least 1");
    }

    void operator()(auto&&... ts) const {
        msg::FunctionCall<typename WebFront::BufferPolicy> command;
        websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(command.header().data()), command.header().size())};
        command.encodeParameter(name, frame);
        ((command.encodeParameter(std::forward<decltype(ts)>(ts), frame)), ...);
        command.setParametersCount(static_cast<std::uint8_t>(1 + sizeof...(ts)));
        const auto message = std::make_shared<const std::vector<std::byte>>(payload(frame));

        // The links are shared while used, and released after the loop, outside every lock.
        const auto links = webFront.connectedLinks();
        if (!maxPending) {
            for (const auto& [id, link] : links)
                link->sendFrame(websocket::Frame<Net>{std::span(*message)});
            return;
        }
        for (const auto& [id, link] : links)
            offer(state, id, link, message, *maxPending);
        forgetAbsent(links);
    }

private:
    std::string                name;
    WebFront&                  webFront;
    std::optional<std::size_t> maxPending;
    std::shared_ptr<State>     state = std::make_shared<State>();

    static std::vector<std::byte> payload(const websocket::Frame<Net>& frame) {
        std::vector<std::byte> bytes;
        const auto             buffers = frame.toBuffers();
        for (auto buffer = std::next(buffers.begin()); buffer != buffers.end(); ++buffer) {  // after the frame header
            const auto* first = static_cast<const std::byte*>(buffer->data());
            bytes.insert(bytes.end(), first, first + buffer->size());
        }
        return bytes;
    }

    static void offer(const std::shared_ptr<State>& state, WebLinkId id, const std::shared_ptr<Link>& link, Message message, std::size_t limit) {
        std::shared_ptr<Slot> slot;
        {
            std::scoped_lock lock{state->mutex};
            auto&            entry = state->slots[id];
            // A reused identifier names another connection: what was held for the former one is dropped.
            if (!entry || entry->link.owner_before(link) || link.owner_before(entry->link))
                entry = std::make_shared<Slot>(Slot{link, nullptr, false});
            entry->held = std::move(message);
            if (entry->sending)
                return;  // its sender will queue this newer call instead of the one it replaces
            entry->sending = true;
            slot           = entry;
        }
        send(state, slot, link, limit);
    }

    // Queues the held call while fewer than `limit` frames await the browser, then waits for room. Only one sender per
    // slot runs at a time, so the browser receives the calls in order.
    static void send(const std::shared_ptr<State>& state, const std::shared_ptr<Slot>& slot, const std::shared_ptr<Link>& link, std::size_t limit) {
        for (;;) {
            Message message;
            {
                std::scoped_lock lock{state->mutex};
                if (!slot->held) {
                    slot->sending = false;
                    return;
                }
                // The waiter runs outside the link's locks, on the thread whose write makes room, or when the link
                // closes or is destroyed.
                const auto waiter = link->whenWritesAtMost(limit - 1, [state, slot, weak = std::weak_ptr(link), limit] {
                    if (const auto alive = weak.lock()) {
                        send(state, slot, alive, limit);
                        return;
                    }
                    std::scoped_lock gone{state->mutex};
                    slot->held    = nullptr;
                    slot->sending = false;
                });
                if (waiter != 0)
                    return;  // the waiter now sends
                message = std::move(slot->held);
            }
            // Outside the mutex: a write completing at once may wake the waiter of another broadcast.
            link->sendFrame(websocket::Frame<Net>{std::span(*message)});
        }
    }

    void forgetAbsent(const std::vector<std::pair<WebLinkId, std::shared_ptr<Link>>>& links) const {
        std::scoped_lock lock{state->mutex};
        std::erase_if(state->slots, [&](const auto& entry) {
            return !entry.second->sending && std::ranges::find(links, entry.first, &std::pair<WebLinkId, std::shared_ptr<Link>>::first) == links.end();
        });
    }
};

}  // namespace webfront
