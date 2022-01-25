/// @file WebFront.hpp
/// @date 18/01/2022 19:34:27
/// @author Ambroise Leclerc
/// @brief WebFront UI main objet
#pragma once
#include "HTTPServer.hpp"
#include "WebSocket.hpp"
#include "details/HexDump.hpp"

#include <experimental/net>
#include <future>
#include <string_view>
#include <system_error>

namespace webfront {

class TCPNetworkingTS {
public:
    using Acceptor = std::experimental::net::ip::tcp::acceptor;
    using Endpoint = std::experimental::net::ip::tcp::endpoint;
    using IoContext = std::experimental::net::io_context;
    using Resolver = std::experimental::net::ip::tcp::resolver;
    using Socket = std::experimental::net::ip::tcp::socket;

    // Buffers
    using ConstBuffer = std::experimental::net::const_buffer;
    using MutableBuffer = std::experimental::net::mutable_buffer;

    static MutableBuffer Buffer(void* d, size_t s) noexcept { return { d, s }; }
    static ConstBuffer buffer(const void* d, size_t s) noexcept { return { d, s }; }
    static MutableBuffer Buffer(const MutableBuffer& b) noexcept { return b; }
    static MutableBuffer Buffer(const MutableBuffer& b, size_t s) noexcept { return { b.data(), std::min(b.size(), s) }; }
    static ConstBuffer Buffer(const ConstBuffer& b) noexcept { return b; }
    static ConstBuffer Buffer(const ConstBuffer& b, size_t s) noexcept { return { b.data(), std::min(b.size(), s) }; }
    template<typename T, size_t S> static MutableBuffer Buffer(T(&data)[S]) noexcept { return ToMutableBuffer(data, S); }
    template<typename T, size_t S> static ConstBuffer Buffer(const T(&data)[S]) noexcept { return ToConstBuffer(data, S); }
    template<typename T, size_t S> static MutableBuffer Buffer(std::array<T, S>& data) noexcept { return ToMutableBuffer(data.data(), S); }
    template<typename T, size_t S> static ConstBuffer Buffer(std::array<const T, S>& data) noexcept { return ToConstBuffer(data.data(), data.size()); }
  
    struct Error {
        static inline auto OperationAborted = std::experimental::net::error::operation_aborted;
    };

private:
    template<typename T> static MutableBuffer ToMutableBuffer(T* data, size_t s) { return { s ? data : nullptr, s * sizeof(T) }; }
    template<typename T> static ConstBuffer ToConstBuffer(const T* data, size_t s) { return { s ? data : nullptr, s * sizeof(T) }; }

};

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
    http::Server<TCPNetworkingTS> httpServer;

private:

};
} // namespace webfront