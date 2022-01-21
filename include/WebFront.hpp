#pragma once
#include "HTTPServer.hpp"
#include "WebSocket.hpp"

#include <future>
#include <string_view>


namespace webfront {
class UI /* : public std::enable_shared_from_this<UI> */ {
public:
    UI(std::string_view port) : httpServer("0.0.0.0", port) {
        //        auto self(shared_from_this());
        httpServer.webSockets.onConnected = [](std::shared_ptr<websocket::WebSocket> ws) {
        };
    }


    void run() { httpServer.run(); }
    void runOne() { httpServer.runOne(); }
    auto runAsync() { return std::async(std::launch::async, &UI::run, this); }
private:
    http::Server httpServer;

private:

};
} // namespace webfront