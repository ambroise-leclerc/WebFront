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
#include <string_view>

namespace webfront {

template<typename NetProvider>
class BasicUI {
    using WebSocketPtr = std::shared_ptr<websocket::WebSocket<NetProvider>>;
    http::Server<NetProvider> httpServer;

    enum class Command { Handshake = 72 };
    enum class JSEndian { little = 0, big = 1, mixed = little + big };

public:
    BasicUI(std::string_view port, std::filesystem::path docRoot = ".") : httpServer("0.0.0.0", port, docRoot) {
        httpServer.webSockets.onOpen([this](WebSocketPtr webSocket) {
            log::debug("onOpen");
            webSocket->onMessage([webSocket](std::string_view text) {
                log::debug("onMessage(text) :{}", text);
                webSocket->write("This is my response");
            });
            webSocket->onMessage([this, webSocket](std::span<const std::byte> data) {
                switch (static_cast<Command>(data[0])) {
                case Command::Handshake:
                        sameEndian = (std::endian::native == std::endian::little && static_cast<JSEndian>(data[1]) == JSEndian::little)
                        || (std::endian::native == std::endian::big && static_cast<JSEndian>(data[1]) == JSEndian::big);
                        log::info("Platform and client share the same endianness");
                        break;
                }

                log::infoHex("onMessage(binary) :", data);
            });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &BasicUI::run, this); }

private:
    bool sameEndian = true;

private:
};

using UI = BasicUI<networking::TCPNetworkingTS>;
} // namespace webfront