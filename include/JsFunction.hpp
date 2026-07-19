/// @date 04/02/2022 13:15:27
/// @author Ambroise Leclerc
/// @brief A functor which invokes a corresponding javascript function
#pragma once
#include "http/WebSocket.hpp"
#include "tooling/HexDump.hpp"
#include "weblink/WebLink.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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
    JsFunction(std::string_view functionName, WebFront& wf, WebLinkId linkId)
        : name(functionName), webFront(wf), webLinkId(linkId) {}

    auto operator()(auto&&... ts) {
        msg::FunctionCall<typename WebFront::BufferPolicy> command;
        websocket::Frame<typename WebFront::Net> frame{std::span(reinterpret_cast<const std::byte*>(command.header().data()), command.header().size())};
        command.encodeParameter(name, frame);
        (((command.encodeParameter(std::forward<decltype(ts)>(ts), frame))), ...);
        command.setParametersCount(static_cast<std::uint8_t>(1 + sizeof...(ts)));

        auto&& link = webFront.getLink(webLinkId);
        if constexpr (std::is_same_v<Result, detail::IgnoreJsResult>)
            link.sendFrame(std::move(frame));
        else {
            auto [callId, result] = link.template expectResult<Result>();
            command.setCallId(callId);
            if (callId != 0)
                link.sendFrame(std::move(frame));
            return result;
        }
    }

private:
    std::string name;
    WebFront& webFront;
    WebLinkId webLinkId;
};

} // namespace webfront
