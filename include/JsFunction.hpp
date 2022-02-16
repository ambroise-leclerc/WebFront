/// @date 04/02/2022 13:15:27
/// @author Ambroise Leclerc
/// @brief A functor which invokes a corresponding javascript function
#pragma once
#include "http/WebSocket.hpp"
#include "tooling/HexDump.hpp"
#include "weblink/WebLink.hpp"

#include <algorithm>
#include <array>
#include <string_view>

#include <iostream>

namespace webfront {

template<typename WebFront>
class JsFunction {
public:
    JsFunction(std::string_view functionName, WebFront& wf, WebLinkId linkId)
        : name(functionName), webFront(wf), webLinkId(linkId) {}

    void operator()(auto&&... ts) {
        command.setParametersCount(0);
        command.setPayloadSize(0);
        websocket::Frame<typename WebFront::Net> frame{std::span(reinterpret_cast<const std::byte*>(command.header().data()), command.header().size())};
        command.encodeParameter(name, frame);
        ((command.encodeParameter(std::forward<decltype(ts)>(ts), frame)), ...);
        webFront.getLink(webLinkId).sendFrame(std::move(frame));
    }

private:
    std::string name;
    WebFront& webFront;
    WebLinkId webLinkId;
    msg::FunctionCall command;
};

} // namespace webfront
