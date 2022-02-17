/// @date 31/01/2022 21:19:42
/// @author Ambroise Leclerc
/// @brief WebLink represents a connexion to an HTML/JS renderer
#pragma once
#include "../http/WebSocket.hpp"
#include "../tooling/Logger.hpp"
#include "Messages.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace webfront {

using WebLinkId = uint16_t;

struct WebLinkEvent {
    enum class Code { linked, closed, cppFunctionCalled };

    WebLinkEvent(Code eventCode, WebLinkId id, std::string message = {}, std::span<const std::byte> dataView = {})
        : code(eventCode), webLinkId(id), text(std::move(message)), data(dataView) {}
    Code code;
    WebLinkId webLinkId;
    std::string text;
    std::span<const std::byte> data;
};

template<typename Net>
class WebLink {
    websocket::WebSocket<Net> ws;
    WebLinkId id;
    bool sameEndian;
    std::optional<size_t> logSink;
    std::function<void(WebLinkEvent)> eventsHandler;
    std::span<const std::byte> undecodedData; /// Data received but not yet consumed

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler)
        : ws(std::move(socket)), id(webLinkId), eventsHandler(eventHandler) {
        log::debug("New WebLink created with id:{}", id);

        ws.onMessage([this](std::string_view text) {
            log::debug("onMessage(text) :{}", text);
            ws.write("This is my response");
        });
        ws.onMessage([this](std::span<const std::byte> data) {
            log::infoHex("onMessage(binary) :", data);

            switch (static_cast<msg::Command>(data[0])) {
            case msg::Command::handshake: {
                auto command = msg::Handshake::castFromRawData(data);
                sendCommand(msg::Ack{});
                logSink =
                  log::addSinks([this](std::string_view t) { sendCommand(msg::TextCommand(msg::TxtOpcode::debugLog, t)); });
                sameEndian = (std::endian::native == std::endian::little &&
                              static_cast<msg::JSEndian>(command->getEndian()) == msg::JSEndian::little) ||
                             (std::endian::native == std::endian::big &&
                              static_cast<msg::JSEndian>(command->getEndian()) == msg::JSEndian::big);
                eventsHandler({WebLinkEvent::Code::linked, id});
            } break;

            case msg::Command::callFunction: {
                log::info("Function called !");
                auto command = msg::FunctionCall::castFromRawData(data);
                auto [functionName, paramData] = command->getFunctionName();
                try {
                    eventsHandler(WebLinkEvent(WebLinkEvent::Code::cppFunctionCalled, id, functionName, paramData));
                }
                catch (const std::out_of_range&) {
                    msg::FunctionReturn returnValue;
                    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(returnValue.header().data()), returnValue.header().size())};
        
                    //returnValue.encodeParameter()
                    sendFrame(std::move(frame));
                }
                catch (const std::exception& e) {
                    log::info("event cppFunctionCalled failed with exception {}", e.what());
                }
            } break;

            default: break;
            }
        });

        ws.start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;
    WebLink& operator=(const WebLink&) = delete;
    WebLink& operator=(WebLink&&) = delete;

    ~WebLink() {
        log::debug("WebLink destructor");
        if (logSink) log::removeSinks(logSink.value());
    }

    void sendCommand(auto message) { ws.write(message.header(), message.payload()); }
    void sendFrame(websocket::Frame<Net> frame) { ws.write(std::move(frame)); }
};

} // namespace webfront
