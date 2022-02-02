/// @date 31/01/2022 21:19:42
/// @author Ambroise Leclerc
/// @brief WebLink represents a connexion to an HTML/JS renderer
#pragma once
#include "tooling/Logger.hpp"
#include "http/WebSocket.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace webfront {

using WebLinkId = uint16_t;

struct WebLinkEvent {
    WebLinkEvent(Code eventCode, WebLinkId id) : code(eventCode), webLinkId(id) {}
    enum class Code { linked, closed };
    Code code;
    WebLinkId webLinkId;
};

enum class JSEndian : uint8_t { little = 0, big = 1, mixed = little + big };

enum class Command : uint8_t { handshake, ack, debugLog, textCommand };
namespace msg {

struct Ack {
    std::span<const std::byte> header() { return std::span(reinterpret_cast<const std::byte*>(&head), sizeof(Header)); }
    std::span<const std::byte> payload() { return {}; }

private:
    struct Header {
        Command command = Command::ack;
        JSEndian endian = std::endian::native == std::endian::little ? JSEndian::little : JSEndian::big;
    } head;
    static_assert(sizeof(Header) == 2, "Ack header has to be 2 bytes long");

};

struct DebugLog {
    DebugLog(std::string_view text) : payloadSpan{std::span(reinterpret_cast<const std::byte*>(text.data()), text.size())} {
        head.textLen = static_cast<uint16_t>(text.size());
    }
    std::span<const std::byte> header() { return std::span(reinterpret_cast<const std::byte*>(&head), sizeof(Header)); }
    std::span<const std::byte> payload() { return payloadSpan; }

private:
    struct Header {
        Command command = Command::debugLog;
        std::array<uint8_t, 3> padding = {};
        uint16_t textLen;
    } head;
    static_assert(sizeof(Header) == 6, "DebugLog header has to be 6 bytes long");

    std::span<const std::byte> payloadSpan;
};

/// Commands with a text payload
struct TextCommand {
    enum class Opcode : uint8_t { debugLog, injectScript };
    TextCommand(std::string_view text, Opcode opcode)
        : head{.opcode = opcode, .lengthHi = static_cast<uint8_t>(text.size() >> 8), .lengthLo = static_cast<uint8_t>(text.size())},
          payloadSpan{std::span(reinterpret_cast<const std::byte*>(text.data()), text.size())} {}

    std::span<const std::byte> header() { return std::span(reinterpret_cast<const std::byte*>(&head), sizeof(Header)); }
    std::span<const std::byte> payload() { return payloadSpan; }
private:
    struct Header {
        Command command = Command::textCommand;
        Opcode opcode;
        uint8_t lengthHi, lengthLo; // Payload length = (256 * lengthHi) + lengthLo
    } head;
    static_assert(sizeof(Header) == 4, "TextCommand header has to be 4 bytes long");

    std::span<const std::byte> payloadSpan;
};
} // namespace msg

template<typename Net>
class WebLink {
    websocket::WebSocket<Net> ws;
    WebLinkId id;
    bool sameEndian;
    std::optional<size_t> logSink;
    std::function<void(WebLinkEvent)> eventsHandler;

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler)
        : ws(std::move(socket)), id(webLinkId), eventsHandler(eventHandler) {
        log::debug("New WebLink created with id:{}", id);

        ws.onMessage([this](std::string_view text) {
            log::debug("onMessage(text) :{}", text);
            ws.write("This is my response");
        });
        ws.onMessage([this](std::span<const std::byte> data) {
            switch (static_cast<Command>(data[0])) {
            case Command::handshake:
                sendCommand(msg::Ack{});
                logSink = log::addSinks([this](std::string_view t) { sendCommand(msg::DebugLog(t)); });
                sameEndian = (std::endian::native == std::endian::little && static_cast<JSEndian>(data[1]) == JSEndian::little) ||
                             (std::endian::native == std::endian::big && static_cast<JSEndian>(data[1]) == JSEndian::big);
                log::info("Endianness - platform:{}, client:{}, identical:{}", std::endian::native == std::endian::little ? "little" : "big",
                          static_cast<JSEndian>(data[1]) == JSEndian::little ? "little" : "big", sameEndian);
                eventsHandler(WebLinkEvent(WebLinkEvent::Code::linked, id));
                break;
            default: break;
            }
            log::infoHex("onMessage(binary) :", data);
        });

        ws.start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;
    

    ~WebLink() {
        log::debug("WebLink destructor");
        if (logSink) log::removeSinks(logSink.value());
    }

private:
    void sendCommand(auto message) { ws.write(message.header(), message.payload()); }
};

} // namespace webfront