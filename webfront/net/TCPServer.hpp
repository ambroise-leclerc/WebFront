#pragma once

#include "Socket.hpp"

#include <iostream>
#include <memory>
#include <span>

namespace webfront {
namespace net {

class TCPClient {
public:
    std::function<void(std::span<uint8_t>)> onReceive;
};

template<typename SockLib = SocketLib, typename IPv = IPv4, size_t maxDatagramSize = 1452>
class TCPServer : private Socket<SockLib, IPv> {
    using Sock = Socket<SockLib, IPv>;

public:
    TCPServer(uint16_t port) {
        Sock::sock = socket(IPv::aiFamily, SOCK_STREAM, 0);
        if (Sock::sock < 0) throw SocketException("Error opening socket");

        IPv::setPort(port);
        Sock::bind();
        Sock::listen();

        std::cout << "waiting clients on port " << port << '\n';
        Sock::accept();
        std::cout << "end\n";
    }

    void start() {        
    }

public:
    std::function<void(std::shared_ptr<TCPClient>)> onNewClient;
};


} // namespace net
} // namespace webfront