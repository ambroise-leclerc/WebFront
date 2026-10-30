// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief A JavaScript function called on every connected browser (BasicWF::broadcast).
#pragma once
#include "http/WebSocket.hpp"
#include "weblink/CallFrame.hpp"
#include "weblink/Messages.hpp"
#include "weblink/WebLink.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace webfront {

/// How a broadcast treats a browser that reads slower than it is written.
struct BroadcastOptions {
    /// Without a value, every call is queued for every browser. With one, a call is queued for a browser only while
    /// fewer calls of this function than that await its network; otherwise it replaces the call of this function held
    /// back for that browser, queued as soon as fewer await: a slow browser skips intermediate calls but always
    /// receives the latest. Other frames, such as other functions' calls, do not count. Must be > 0.
    std::optional<std::size_t> maxPendingWrites{};
};

/// Calls a JavaScript function on each started browser, fire-and-forget, from any thread. Each call encodes its
/// arguments once into a frame that every browser shares. The calls held back for a slow browser belong to its
/// connection, so the object holds no state: it may be a temporary, but must not outlive its application.
template <typename WebFront>
class Broadcast {
    using Net  = typename WebFront::Net;
    using Link = WebLink<Net, typename WebFront::BufferPolicy>;

public:
    Broadcast(std::string_view functionName, WebFront& wf, BroadcastOptions options = {})
        : name(functionName), webFront(wf), maxPending(options.maxPendingWrites) {
        if (maxPending && *maxPending == 0)
            throw std::invalid_argument("BroadcastOptions::maxPendingWrites must be at least 1");
    }

    void operator()(auto&&... ts) const {
        msg::FunctionCall<typename WebFront::BufferPolicy> command;
        auto                                               frame = detail::encodeCall<Net>(command, name, std::forward<decltype(ts)>(ts)...);
        // The sending may be deferred until a starting browser's onUIStarted handlers return: it owns what it uses.
        webFront.sendToStartedLinks([shared = std::move(frame).share(), key = name, limit = maxPending](Link& link) {
            if (limit)
                link.sendLatest(shared, key, *limit);
            else
                link.sendFrame(shared);
        });
    }

private:
    std::string                name;
    WebFront&                  webFront;
    std::optional<std::size_t> maxPending;
};

}  // namespace webfront
