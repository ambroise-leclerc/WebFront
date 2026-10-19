/// @date   13/08/2015
/// @author Ambroise Leclerc
///
/// UDP/IP(v4/v6) socket class for MSVC, Linux and Cygwin.
#pragma once
#include "Socket.hpp"

#include <span>

namespace webfront {
namespace net {

template<typename SockLib = SocketLib, typename IPv = IPv4, size_t maxDatagramSize = 1452>
class UDPSocket : public Socket<SockLib, IPv> {
    using Sock = Socket<SockLib, IPv>;

public:
    static constexpr size_t maxMessageSize = maxDatagramSize;

public:
    /// Initializes an UDP socket and associates it with the given address
    /// @param address host name of dotted-decimal IPv4 address or IPv6 hex address
    /// @param service service name or port number (i.e. "80" or "http")
    void initialize(const std::string& address, const std::string& service) {
        addrinfo* addrInfo;
        addrinfo hints = {};

        hints.ai_family = IPv::aiFamily;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;

        auto status = getaddrinfo(address.c_str(), service.c_str(), &hints, &addrInfo);
        if (status != 0) throw SocketException(std::string("In UDPSocket::initialize getaddrinfo : ") + gai_strerror(status));

        ScopeExitGuard releaseAddrinfo([addrInfo]() { freeaddrinfo(addrInfo); });

        Sock::sock = socket(addrInfo->ai_family, addrInfo->ai_socktype, addrInfo->ai_protocol);
        if (Sock::sock == SockLib::invalidSocket)
            throw SocketException(std::string("In UDPSocket::initialize socket : ") + SockLib::getErrorText());

        Sock::setOption(Sock::Option::ReuseAddr, int(1));

        IPv::setPort(addrInfo->ai_addr);
        IPv::sockAddr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(Sock::sock, reinterpret_cast<const sockaddr*>(&this->sockAddr), sizeof(IPv::sockAddr)) ==
            SockLib::socketError) {
            auto errorText = SockLib::getErrorText();
            SocketLib::closeSocket(Sock::sock);
            throw SocketException(std::string("In UDPSocket::initialize bind : ") + errorText);
        }

        IPv::sockAddr = *(reinterpret_cast<decltype(IPv::sockAddr)*>(addrInfo->ai_addr));

        if (::connect(Sock::sock, reinterpret_cast<sockaddr*>(&this->sockAddr), sizeof(IPv::sockAddr)) == SockLib::socketError)
            throw SocketException(std::string("In UDPSocket::initialize connect : ") + SockLib::getErrorText());
    }

    template<typename Rep, typename Period>
    void setReceiveTimeout(std::chrono::duration<Rep, Period> timeout) const {
        auto uSec = std::chrono::duration_cast<std::chrono::microseconds>(timeout);
        Sock::setOption(Sock::Option::RcvTimeout, SockLib::toTimeout(uSec));
    }

    void enlargeReceiveBufferSize(size_t size) const {
        auto actualBufferSize = static_cast<size_t>(Sock::template getOption<int>(Sock::Option::RcvBufferSize));
        if (actualBufferSize < size) Sock::setOption(Sock::Option::RcvBufferSize, static_cast<int>(size));
    }

    /// Send data
    /// @tparam Buffer a type which exposes data() and size() members
    /// @param buffer data to be sent
    template<typename Buffer>
    void send(Buffer buffer) const {
        auto data = reinterpret_cast<const char*>(buffer.data());
        auto remainingDataToSend = static_cast<size_t>(buffer.size());
        while (remainingDataToSend > 0) {
            auto sent = ::send(Sock::sock, data, static_cast<int>(std::min(maxDatagramSize, remainingDataToSend)), 0);
            if (sent == SockLib::socketError)
                throw SocketException(std::string("In UDPSocket::send : ") + SockLib::getErrorText());
            remainingDataToSend -= sent;
            data += sent;
        }
    }

    /// Receive a datagram
    /// @param buffer pointer on the buffer which will receive the data
    /// @return number of received bytes
    size_t receive(std::span<uint8_t> buffer) const {
        auto received = ::recv(Sock::sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);

        if (received == SockLib::socketError) {
            auto error = Sock::getLastError();
            auto errorNo = static_cast<typename SockLib::Error>(error);
            if ((errorNo == SockLib::Error::Timeout) || (errorNo == SockLib::Error::WouldBlock))
                throw TimeoutException("Timeout on UDPSocket::receive");

            throw SocketException(std::string("In UDPSocket::receive : ") + SockLib::getErrorText(error));
        }

        return received;
    }
};

} // namespace net
} // namespace webfront

