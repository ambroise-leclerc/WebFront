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

template<typename Net>
class WebLink {
    enum class Command { Handshake = 72 };
    enum class JSEndian { little = 0, big = 1, mixed = little + big };

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
            case Command::Handshake:
                sameEndian = (std::endian::native == std::endian::little && static_cast<JSEndian>(data[1]) == JSEndian::little) ||
                             (std::endian::native == std::endian::big && static_cast<JSEndian>(data[1]) == JSEndian::big);
                log::info("Endianness - platform:{}, client:{}, identical:{}", std::endian::native == std::endian::little ? "little" : "big",
                          static_cast<JSEndian>(data[1]) == JSEndian::little ? "little" : "big", sameEndian);
                // webSocket.write()
                break;
            }

            log::infoHex("onMessage(binary) :", data);
        });

        ws.start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;

    ~WebLink() { log::debug("WebLink destructor"); }
};

template<typename Net>
class BasicUI {
public:
    BasicUI(std::string_view port, std::filesystem::path docRoot = ".") : httpServer("0.0.0.0", port, docRoot), idCounter(0) {
        httpServer.onUpgrade([this](typename Net::Socket&& socket, http::Protocol protocol) {
            if (protocol == http::Protocol::WebSocket) {
                bool inserted;
                do {
                    std::tie(std::ignore, inserted) =
                      webLinks.try_emplace(idCounter, std::move(socket), idCounter, [this](WebLinkEvent event) { onEvent(event); });
                    ++idCounter;
                } while (!inserted);
            }
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &BasicUI::run, this); }

private:
    http::Server<Net> httpServer;
    std::map<WebLinkId, WebLink<Net>> webLinks;
    WebLinkId idCounter;

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