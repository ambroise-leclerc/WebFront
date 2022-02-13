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
            switch (static_cast<msg::Command>(data[0])) {
            case msg::Command::handshake: {
                auto command = msg::Handshake::castFromRawData(data);
                sendCommand(msg::Ack{});
                logSink = log::addSinks([this](std::string_view t) { sendCommand(msg::TextCommand(msg::TxtOpcode::debugLog, t)); });
                sameEndian = (std::endian::native == std::endian::little && static_cast<msg::JSEndian>(command->getEndian()) == msg::JSEndian::little) ||
                             (std::endian::native == std::endian::big && static_cast<msg::JSEndian>(command->getEndian()) == msg::JSEndian::big);
                eventsHandler({WebLinkEvent::Code::linked, id});
            } break;

            case msg::Command::callFunction: {
                log::info("Function called !");
                auto command = msg::FunctionCall::castFromRawData(data);
                auto [functionName, paramData] = command->getFunctionName();
                eventsHandler(WebLinkEvent(WebLinkEvent::Code::cppFunctionCalled, id, functionName, paramData));

            } break;

            default: break;
            }
            log::infoHex("onMessage(binary) :", data);
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

    template<typename T>
    void extractNext(T& value) {
        size_t bytesDecoded;
        std::tie(value, bytesDecoded) = decodeParameter<T>(undecodedData);
        undecodedData = undecodedData.subspan(bytesDecoded);
    }

private:
    void callCppFunction(size_t parametersCount, std::span<const std::byte> data) {
        if (parametersCount > 0) {
            auto [functionName, bytesDecoded] = decodeParameter<std::string>(data);
            undecodedData = data.subspan(bytesDecoded);
            log::debug("Function called : {}", functionName);
            eventsHandler({WebLinkEvent::Code::cppFunctionCalled, id, functionName});
        }
    }

    template<typename T>
    std::tuple<T, size_t> decodeParameter(std::span<const std::byte> data) {
        if (data.size() == 0) throw std::runtime_error("Not enough data for WebLink::decodeParameter");
        auto codedType = static_cast<msg::CodedType>(data[0]);
        switch (codedType) {
        case msg::CodedType::smallString:
            if constexpr (std::is_same_v<T, std::string>) {
                if (data.size() < 1) throw std::runtime_error("Erroneous data feeded to WebLink::decodeParameter");
                auto size = static_cast<size_t>(data[1]);
                if (data.size() < 2 + size) throw std::runtime_error("Erroneous data feeded to WebLink::decodeParameter");
                return {std::string(reinterpret_cast<const char*>(&data[2]), size), 2 + size};
            }

            break;
        default: return {T{}, 0};
        }
    }
};

} // namespace webfront