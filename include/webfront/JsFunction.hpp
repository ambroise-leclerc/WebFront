/// @date 04/02/2022 13:15:27
/// @author Ambroise Leclerc
/// @brief A functor which invokes a corresponding javascript function
#pragma once
#include "http/WebSocket.hpp"
#include "tooling/HexDump.hpp"
#include "weblink/CallFrame.hpp"
#include "weblink/WebLink.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <iostream>

namespace webfront {

namespace detail {
struct IgnoreJsResult {};
}

template<typename WebFront, typename Result = detail::IgnoreJsResult>
class JsFunction {
public:
    JsFunction(std::string_view functionName, WebFront& wf, WebLinkId linkId, WebLinkGeneration connectionGeneration)
        : name(functionName), webFront(wf), webLinkId(linkId), generation(connectionGeneration) {}

    auto operator()(auto&&... ts) {
        msg::FunctionCall<typename WebFront::BufferPolicy> command;
        auto                                               frame = detail::encodeCall<typename WebFront::Net>(command, name, std::forward<decltype(ts)>(ts)...);

        // A browser whose connection closed is gone: a call to it is dropped, and a typed call fails.
        // The link is shared while used: a release on the server thread cannot destroy it meanwhile.
        const auto link = webFront.findLink(webLinkId, generation);
        if constexpr (std::is_same_v<Result, detail::IgnoreJsResult>) {
            if (link)
                link->sendFrame(std::move(frame));
        } else {
            if (!link) {
                std::promise<Result> closed;
                closed.set_exception(std::make_exception_ptr(std::runtime_error("Browser connection closed")));
                return closed.get_future();
            }
            auto [callId, result] = link->template expectResult<Result>();
            command.setCallId(callId);
            if (callId != 0)
                link->sendFrame(std::move(frame));
            // Structured bindings are not implicitly movable on return, so a move-only future would be
            // copied here. Newer libc++ accepts it; Apple Clang rejects it outright.
            return std::move(result);
        }
    }

private:
    std::string name;
    WebFront& webFront;
    WebLinkId webLinkId;
    WebLinkGeneration generation;
};

} // namespace webfront
