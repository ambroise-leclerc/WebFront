/// @file WebFront.hpp
/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include "HTTPServer.hpp"
#include "WebSocket.hpp"
#include "details/HexDump.hpp"

#include <future>
#include <string_view>


namespace webfront {
class UI {
public:
    UI(std::string_view port) : httpServer("0.0.0.0", port) {
        httpServer.webSockets.onOpen([](std::shared_ptr<websocket::WebSocket> webSocket) {
            webSocket->onMessage([webSocket](std::string_view text) {
                std::cout << "onMessage(text) : " << text << "\n";
                webSocket->write("This is my response");
            });

            webSocket->onMessage([webSocket](std::span<const std::byte> data) {
                std::cout << "onMessage(binary) : " << utils::HexDump(data) << "\n";
            });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &UI::run, this); }
private:
    http::Server httpServer;

private:

};
} // namespace webfront