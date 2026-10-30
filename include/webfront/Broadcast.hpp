// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief A JavaScript function called on every connected browser (BasicWF::broadcast).
#pragma once
#include "http/WebSocket.hpp"
#include "weblink/CallFrame.hpp"
#include "weblink/Messages.hpp"
#include "weblink/WebLink.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront {

/// How a broadcast treats a browser that reads slower than it is written.
struct BroadcastOptions {
    /// Without a value, every call is queued for every browser. With one, a call is queued for a browser only while
    /// fewer calls of this function than that await its network; otherwise it replaces the call of this function held
    /// back for that browser, queued as soon as fewer await: a slow browser skips intermediate calls but always
    /// receives the latest. Other frames, such as other functions' calls, do not count, and a held call may reach the
    /// browser after them. Calls of one function with different limits share the browser's queue: each call applies
    /// its own, and a browser holds at most one call per function, which a call to hold replaces whatever limit held
    /// the previous one. Only broadcasts count: `ui.jsFunction` calls of the same name are other frames. Must be > 0.
    std::optional<std::size_t> maxPendingWrites{};
};

namespace detail {
/// One broadcast call: its frame, shared by every browser, and how to send it.
template <typename Net>
struct BroadcastCall {
    std::shared_ptr<const websocket::Frame<Net>> frame;
    std::string                                  function;
    std::optional<std::size_t>                   maxPendingWrites;

    void sendTo(auto& link) const {
        if (maxPendingWrites)
            link.sendLatest(frame, function, *maxPendingWrites);
        else
            link.sendFrame(frame);
    }

    /// Keeps the call for a browser whose onUIStarted handlers run. With a limit, at most that many calls of the
    /// function are kept: the newest replaces the last one kept, at the end of the calls.
    void deferInto(std::vector<BroadcastCall>& deferred) const {
        if (maxPendingWrites) {
            const auto sameFunction = [this](const BroadcastCall& call) {
                return call.function == function;
            };
            if (static_cast<std::size_t>(std::ranges::count_if(deferred, sameFunction)) >= *maxPendingWrites) {
                const auto last = std::ranges::find_if(deferred.rbegin(), deferred.rend(), sameFunction);
                deferred.erase(std::next(last).base());
            }
        }
        deferred.push_back(*this);
    }
};
}  // namespace detail

/// Calls a JavaScript function on each started browser, fire-and-forget, from any thread. Each call encodes its
/// arguments once into a frame that every browser shares. The calls held back for a slow browser belong to its
/// connection, so the object holds no state: it may be a temporary. It refers to its application: no call may still
/// run when the application's shutdown() or destructor starts.
template <typename WebFront>
class Broadcast {
    using Net = typename WebFront::Net;

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
        webFront.sendToStartedLinks(detail::BroadcastCall<Net>{std::move(frame).share(), name, maxPending});
    }

private:
    std::string                name;
    WebFront&                  webFront;
    std::optional<std::size_t> maxPending;
};

}  // namespace webfront
