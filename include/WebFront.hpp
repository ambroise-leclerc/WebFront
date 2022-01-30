/// @file WebFront.hpp
/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include <details/HexDump.hpp>
#include <http/HTTPServer.hpp>
#include <http/WebSocket.hpp>
#include <networking/TCPNetworkingTS.hpp>

#include <bit>
#include <map>
#include <string_view>
#include <tuple>

namespace webfront {

using WebLinkId = uint16_t;

struct WebLinkEvent {
    enum class Code { closed };
    Code code;
    WebLinkId webLinkId;
};

enum class JSEndian : uint8_t { little = 0, big = 1, mixed = little + big };

enum class Command : uint8_t { handshake, ack, debugLog };
namespace msg {

struct Ack {
    struct Header {
        Command command = Command::ack;
        JSEndian endian = std::endian::native == std::endian::little ? JSEndian::little : JSEndian::big;
    } head;
    static_assert(sizeof(Header) == 2, "Ack header needs to be 2 bytes long");

    std::span<const std::byte> header() { return std::span(reinterpret_cast<const std::byte*>(&head), sizeof(Header)); }
    std::span<const std::byte> payload() { return {}; }
};

struct DebugLog {
    DebugLog(std::string_view text) : payloadSpan{std::span(reinterpret_cast<const std::byte*>(text.data()), text.size())} {
        head.textLen = static_cast<uint16_t>(text.size());
    }
    std::span<const std::byte> header() { return std::span(reinterpret_cast<const std::byte*>(&head), sizeof(Header)); }
    std::span<const std::byte> payload() { return payloadSpan; }

    struct Header {
        Command command = Command::debugLog;
        uint8_t padding[3] = {};
        uint16_t textLen;
    } head;
    static_assert(sizeof(Header) == 6, "DebugLog header needs to be 6 bytes long");

protected:
    std::span<const std::byte> payloadSpan;

};
// static_assert(sizeof(DebugLog) - sizeof(std::span<const std::byte>) == 4, "DebugLog header need to be 4 bytes long");

} // namespace msg

template<typename Net>
class WebLink {


    websocket::WebSocket<Net> ws;
    WebLinkId id;
    bool sameEndian;

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)>) : ws(std::move(socket)), id(webLinkId) {
        log::debug("New WebLink created with id:{}", id);

        ws.onMessage([this](std::string_view text) {
            log::debug("onMessage(text) :{}", text);
            ws.write("This is my response");
        });
        ws.onMessage([this](std::span<const std::byte> data) {
            switch (static_cast<Command>(data[0])) {
            case Command::handshake:
                sameEndian = (std::endian::native == std::endian::little && static_cast<JSEndian>(data[1]) == JSEndian::little) ||
                             (std::endian::native == std::endian::big && static_cast<JSEndian>(data[1]) == JSEndian::big);
                log::info("Endianness - platform:{}, client:{}, identical:{}", std::endian::native == std::endian::little ? "little" : "big",
                          static_cast<JSEndian>(data[1]) == JSEndian::little ? "little" : "big", sameEndian);
                sendCommand(msg::Ack{});
                sendCommand(msg::DebugLog{"Debug text"});
                break;
            }

            log::infoHex("onMessage(binary) :", data);
        });

        ws.start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;

    ~WebLink() { log::debug("WebLink destructor"); }

private:
    void sendCommand(auto message) {
        log::infoHex("header", message.header());
        log::infoHex("payload:", message.payload());
        ws.write(message.header(), message.payload());
    }
};

template<typename Net>
class BasicUI {
public:
    BasicUI(std::string_view port, std::filesystem::path docRoot = ".") : httpServer("0.0.0.0", port, docRoot), idsCounter(0) {
        httpServer.onUpgrade([this](typename Net::Socket&& socket, http::Protocol protocol) {
            if (protocol == http::Protocol::WebSocket)
                for (bool inserted = false; !inserted; ++idsCounter)
                    std::tie(std::ignore, inserted) =
                      webLinks.try_emplace(idsCounter, std::move(socket), idsCounter, [this](WebLinkEvent event) { onEvent(event); });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &BasicUI::run, this); }

private:
    http::Server<Net> httpServer;
    std::map<WebLinkId, WebLink<Net>> webLinks;
    WebLinkId idsCounter;

private:
    void onEvent(WebLinkEvent event) {
        log::debug("onEvent");
        switch (event.code) {
        case WebLinkEvent::Code::closed: webLinks.erase(event.webLinkId); break;
        }
    }
};

using UI = BasicUI<networking::TCPNetworkingTS>;
} // namespace webfront