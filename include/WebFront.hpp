/// @file WebFront.hpp
/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include <http/HTTPServer.hpp>
#include <http/WebSocket.hpp>
#include <details/HexDump.hpp>
#include <networking/TCPNetworkingTS.hpp>

#include <future>
#include <string_view>
#include <system_error>

namespace webfront {

template<typename NetProvider>
class BasicUI {
    using WebSocketPtr = std::shared_ptr<websocket::WebSocket<NetProvider>>;
    http::Server<NetProvider> httpServer;

public:
    BasicUI(std::string_view port) : httpServer("0.0.0.0", port) {
        httpServer.webSockets.onOpen([](WebSocketPtr webSocket) {
            webSocket->onMessage([webSocket](std::string_view text) {
                std::cout << "onMessage(text) : " << text << "\n";
                webSocket->write("This is my response");
            });

            webSocket->onMessage([webSocket](std::span<const std::byte> data) { std::cout << "onMessage(binary) : " << utils::HexDump(data) << "\n"; });
        });
    }

    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &BasicUI::run, this); }

private:
    

private:
};

using UI = BasicUI<networking::TCPNetworkingTS>;
} // namespace webfront