// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The frame of a C++ call to a JavaScript function, shared by every way of making one.
#pragma once
#include "../http/WebSocket.hpp"
#include "Messages.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace webfront::detail {
/// The frame calling `name` with `arguments`. It borrows from `command` and from the arguments: send it while they
/// live, or own it first (Frame::freeze() or share()).
template <typename Net, typename Policy>
websocket::Frame<Net> encodeCall(msg::FunctionCall<Policy>& command, const std::string& name, auto&&... arguments) {
    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(command.header().data()), command.header().size())};
    command.encodeParameter(name, frame);
    (command.encodeParameter(std::forward<decltype(arguments)>(arguments), frame), ...);
    command.setParametersCount(static_cast<std::uint8_t>(1 + sizeof...(arguments)));
    return frame;
}
}  // namespace webfront::detail
