/// @date   13/08/2015
/// @author Ambroise Leclerc
///
/// socket base classes for MSVC, Linux and Cygwin.
///
#pragma once
#include <system/SocketLibOS.hpp>

#include <chrono>
#include <exception>
#include <string>
#include <vector>

namespace webfront {
namespace net {

class SocketLib;
class IPv4;
class SocketException;

template<typename SockLib = SocketLib, typename IPv = IPv4>
class Socket : public SockLib, public IPv {
public:
    Socket() noexcept : sock(SockLib::invalidSocket) {}

    ~Socket() {
        if (sock != SockLib::invalidSocket) {
            close();
        }
    }

    /// Shutdown and close a socket
    void close() {
        auto toClose = sock;
        sock = SockLib::invalidSocket;
        if (toClose != SockLib::invalidSocket) {
            ::shutdown(toClose, static_cast<int>(SockLib::Shutdown::ReadWrite));
            SockLib::closeSocket(toClose);
        }
    }

    /// Ping the target address
    /// @return <0> number of replies, 0 if host is unreachable
    /// @return <1> round trip time
    std::tuple<uint8_t, std::chrono::microseconds> ping() const noexcept { return SockLib::ping(IPv::getIP()); }

    static constexpr auto nbReplies = 0;
    static constexpr auto roundtripTime = 0;

protected:
    typename SockLib::SocketType sock;

    enum class Option { ReuseAddr = SO_REUSEADDR, RcvTimeout = SO_RCVTIMEO, RcvBufferSize = SO_RCVBUF, TTL = IP_TTL };
    enum class Level : uint16_t { Socket = SOL_SOCKET, IP = IPPROTO_IP };

    /// Set socket option (i.e. setsockopt)
    /// @param option option to be modified
    /// @param value value which will be affected to option
    /// @param level
    template<typename ValueType>
    void setOption(Option option, ValueType value, Level level = Level::Socket) const {
        auto errorCode = setsockopt(sock, static_cast<int>(level), static_cast<int>(option),
                                    reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(ValueType)));
        if (errorCode == SockLib::socketError)
            throw SocketException(std::string("In Socket::setOption : ") + SockLib::getErrorText());
    }

    /// Get socket option (i.e. getsockopt)
    /// @param option option from which the value will be get
    /// @tparam ValueType type of option's value
    template<typename ValueType>
    ValueType getOption(Option option, Level level = Level::Socket) const {
        ValueType value;
        auto valueSize = static_cast<int>(sizeof(ValueType));
        auto errorCode = ::getsockopt(sock, static_cast<int>(level), static_cast<int>(option), reinterpret_cast<char*>(&value),
                                      reinterpret_cast<typename SockLib::SockLen*>(&valueSize));
        if (errorCode == SockLib::socketError)
            throw SocketException(std::string("In Socket::getOption : ") + SockLib::getErrorText());

        return value;
    }
};

class IPv4 {
public:
    IPv4() noexcept : sockAddr{aiFamily, 0, INADDR_ANY, {}} {}

    std::string getAddress() const noexcept {
        char ip[INET_ADDRSTRLEN];
        auto addr = sockAddr.sin_addr;
        inet_ntop(aiFamily, &addr, ip, INET_ADDRSTRLEN);
        return ip;
    }

    auto getIP() const noexcept { return sockAddr.sin_addr.s_addr; }

    void setPort(const sockaddr* addr) noexcept { sockAddr.sin_port = reinterpret_cast<const sockaddr_in*>(addr)->sin_port; }

protected:
    sockaddr_in sockAddr;
    static constexpr auto aiFamily = AF_INET;
    static constexpr int protoIcmp = IPPROTO_ICMP;
};

class IPv6 {
public:
    IPv6() noexcept : sockAddr{aiFamily, 0, 0, in6addr_any, 0} {}

    std::string getAddress() const noexcept {
        char ip[INET6_ADDRSTRLEN];
        auto addr = sockAddr.sin6_addr;
        inet_ntop(aiFamily, &addr, ip, INET6_ADDRSTRLEN);
        return ip;
    }

    auto getIP() const noexcept { return sockAddr; }

    void setPort(const sockaddr* addr) noexcept { sockAddr.sin6_port = reinterpret_cast<const sockaddr_in6*>(addr)->sin6_port; }

protected:
    sockaddr_in6 sockAddr;
    static constexpr auto aiFamily = AF_INET6;
    static constexpr int protoIcmp = IPPROTO_ICMPV6;
};

class SocketLib : public SocketLibOS {
public:
    static void closeSocket(SocketType socket) {
        if (!SocketLibOS::closeSocket(socket)) {
            throw SocketException("In SocketLib::closeSocket : " + getErrorText());
        }
    }

    static void shutdown(SocketType socket, Shutdown direction = Shutdown::ReadWrite) {
        if (::shutdown(socket, static_cast<int>(direction)) == socketError) {
            throw SocketException("In SocketLib::shutdown : " + getErrorText());
        }
    }
};

class SocketException : public std::runtime_error {
public:
    explicit SocketException(const std::string& what) : std::runtime_error(what) {}
};



} // namespace net
} // namespace webfront
