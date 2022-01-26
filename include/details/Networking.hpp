/// @file Networking.hpp
/// @date 22/01/2022 16:00:55
/// @author Ambroise Leclerc
/// @brief Some networking classes which provide common access to C++2x NetworkingTS, Mock networking or internal implementation
#pragma once
#include <array>
#include <experimental/net>
#include <span>
#include <string>



/// TCPNetworkingTS : C++2x networking TS from std::experimental::net///  
/// NetworkingMock  : a Networking mock implementation for testing purposes
/// TCPWinsock      : a WInsock implementation for Windows
/// TCPSockets      : a BSD socket implement for Linux 

namespace webfront {

template<typename T>
concept Networking = requires(T) {
    T::Acceptor;
    T::Endpoint;
    T::IoContext;
    T::Resolver;
    T::Socket;
    T::ConstBuffer;
    T::MutableBuffer;
};

namespace buffers {
class MutableBuffer {
public:
    MutableBuffer() noexcept : bufData(0), bufSize(0) {}
    MutableBuffer(void* data, std::size_t size) noexcept : bufData(data), bufSize(size) {}
    void* data() const noexcept { return bufData; }
    size_t size() const noexcept { return bufSize; }
    MutableBuffer& operator+=(size_t n) noexcept {
        size_t offset = n < bufSize ? n : bufSize;
        bufData = static_cast<uint8_t*>(bufData) + offset;
        bufSize -= offset;
        return *this;
    }

private:
    void* bufData;
    std::size_t bufSize;
};

class ConstBuffer {
public:
    ConstBuffer() noexcept : bufData(0), bufSize(0) {}
    ConstBuffer(const void* data, std::size_t size) noexcept : bufData(data), bufSize(size) {}
    ConstBuffer(const MutableBuffer& b) : bufData(b.data()), bufSize(b.size()) {}
    const void* data() const noexcept { return bufData; }
    size_t size() const noexcept { return bufSize; }
    ConstBuffer& operator+=(size_t n) noexcept {
        size_t offset = n < bufSize ? n : bufSize;
        bufData = static_cast<const uint8_t*>(bufData) + offset;
        bufSize -= offset;
        return *this;
    }
    
private:
    const void* bufData;
    std::size_t bufSize;
};

} // namespace buffers

template<typename ConstBufferT = buffers::ConstBuffer, typename MutableBufferT = buffers::MutableBuffer>
class BaseNetworking {
public:
    using ConstBuffer = ConstBufferT;
    using MutableBuffer = MutableBufferT;

    static MutableBuffer Buffer(void* d, size_t s) noexcept { return {d, s}; }
    static ConstBuffer Buffer(const void* d, size_t s) noexcept { return {d, s}; }
    static MutableBuffer Buffer(const MutableBuffer& b) noexcept { return b; }
    static MutableBuffer Buffer(const MutableBuffer& b, size_t s) noexcept { return {b.data(), std::min(b.size(), s)}; }
    static ConstBuffer Buffer(const ConstBuffer& b) noexcept { return b; }
    static ConstBuffer Buffer(const ConstBuffer& b, size_t s) noexcept { return {b.data(), std::min(b.size(), s)}; }
    template<typename T, size_t S>
    static MutableBuffer Buffer(T (&d)[S]) noexcept {
        return ToMutableBuffer(d, S);
    }
    template<typename T, size_t S>
    static ConstBuffer Buffer(const T (&d)[S]) noexcept {
        return ToConstBuffer(d, S);
    }
    template<typename T, size_t S>
    static MutableBuffer Buffer(std::array<T, S>& d) noexcept {
        return ToMutableBuffer(d.data(), S);
    }
    template<typename T, size_t S>
    static ConstBuffer Buffer(std::array<const T, S>& d) noexcept {
        return ToConstBuffer(d.data(), d.size());
    }
    template<typename T, typename Alloc>
    static MutableBuffer Buffer(std::vector<T, Alloc>& d) noexcept {
        return ToMutableBuffer(d.data(), d.size());
    }
    template<typename T, typename Alloc>
    static ConstBuffer Buffer(const std::vector<T, Alloc>& d) noexcept {
        return ToConstBuffer(d.data(), d.size());
    }

    template<typename CharT, typename Traits, typename Alloc>
    static MutableBuffer Buffer(std::basic_string<CharT, Traits, Alloc>& d) noexcept {
        return ToMutableBuffer(&d.front(), d.size());
    }

    template<typename CharT, typename Traits, typename Alloc>
    static ConstBuffer Buffer(const std::basic_string<CharT, Traits, Alloc>& d) noexcept {
        return ToConstBuffer(&d.front(), d.size());
    }

    template<typename CharT, typename Traits>
    static ConstBuffer Buffer(std::basic_string_view<CharT, Traits> d) noexcept {
        return ToConstBuffer(d.data(), d.size());
    }

    template<typename T, size_t S>
    static MutableBuffer Buffer(T (&d)[S], size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static ConstBuffer Buffer(const T (&d)[S], size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static MutableBuffer Buffer(std::array<T, S>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static ConstBuffer Buffer(std::array<const T, S>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, size_t S>
    static ConstBuffer Buffer(const std::array<T, S>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, typename Alloc>
    static MutableBuffer Buffer(std::vector<T, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename T, typename Alloc>
    static ConstBuffer Buffer(const std::vector<T, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(T));
    }

    template<typename CharT, typename Traits, typename Alloc>
    static MutableBuffer Buffer(std::basic_string<CharT, Traits, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    template<typename CharT, typename Traits, typename Alloc>
    static ConstBuffer Buffer(const std::basic_string<CharT, Traits, Alloc>& d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    template<typename CharT, typename Traits>
    static ConstBuffer Buffer(std::basic_string_view<CharT, Traits> d, size_t s) noexcept {
        return Buffer(Buffer(d), s * sizeof(CharT));
    }

    struct Error {
        static inline auto OperationAborted = std::experimental::net::error::operation_aborted;
    };

private:
    template<typename T>
    static MutableBuffer ToMutableBuffer(T* data, size_t s) {
        return {s ? data : nullptr, s * sizeof(T)};
    }
    template<typename T>
    static ConstBuffer ToConstBuffer(const T* data, size_t s) {
        return {s ? data : nullptr, s * sizeof(T)};
    }

protected:
    using super = BaseNetworking;
};

/// TCPNetworkingTS provides access to C++2x networking ts (std::experimental::net)
class TCPNetworkingTS : public BaseNetworking<std::experimental::net::const_buffer, std::experimental::net::mutable_buffer> {
public:
    using Acceptor = std::experimental::net::ip::tcp::acceptor;
    using Endpoint = std::experimental::net::ip::tcp::endpoint;
    using IoContext = std::experimental::net::io_context;
    using Resolver = std::experimental::net::ip::tcp::resolver;
    using Socket = std::experimental::net::ip::tcp::socket;
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename... Args>
    static auto AsyncWrite(Args&&... args) -> decltype(std::experimental::net::async_write(std::forward<Args>(args)...)) {
        return std::experimental::net::async_write(std::forward<Args>(args)...);
    }

    template<typename... Args>
    static auto Write(Args&&... args) -> decltype(std::experimental::net::write(std::forward<Args>(args)...)) {
        return std::experimental::net::write(std::forward<Args>(args)...);
    }
};

class NetworkingMock : public BaseNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;

    template<typename... Args>
    static auto AsyncWrite(Args&&... args) -> void {
        return;
    }
};

} // namespace webfront

/*
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <IPHlpApi.h>
#include <IcmpAPI.h>
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif



#include <array>
#include <chrono>
#include <string>
#include <tuple>

namespace webfront {
namespace net {

namespace {
/// OS abstration layer for socket libraries
class SocketLibOS {
public:
#ifdef _WIN32
    static bool initializeLib() noexcept {
        WSADATA wsa_data;
        int init_value = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        return init_value == 0;
    }

    static void cleanupLib() noexcept { WSACleanup(); }

protected:
    using SocketType = SOCKET;
    using SockLen = int;
    static const SocketType invalidSocket = INVALID_SOCKET;
    static const int socketError = SOCKET_ERROR;
    enum class Shutdown { Read = SD_SEND, Write = SD_RECEIVE, ReadWrite = SD_BOTH };
    enum class Error { Timeout = WSAETIMEDOUT, WouldBlock = WSAEWOULDBLOCK };

    static bool closeSocket(SocketType socket) noexcept { return closesocket(socket) != socketError; }
    static auto getLastError() { return WSAGetLastError(); }
    static std::string getErrorText(bool prependErrorId = true) { return getErrorText(getLastError(), prependErrorId); }
    static std::string getErrorText(int error, bool prependErrorId = true) {
        LPVOID lpMsgBuf;
        auto bufLen = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<LPTSTR>(&lpMsgBuf), 0, nullptr);
        if (bufLen) {
            auto lpMsgStr = reinterpret_cast<LPCSTR>(lpMsgBuf);
            std::string result(lpMsgStr, lpMsgStr + bufLen);
            LocalFree(lpMsgBuf);
            if (prependErrorId) {
                result.insert(0, std::to_string(error) + ":");
            }
            return result;
        }
        return std::string();
    }

    static constexpr auto toTimeout(const std::chrono::microseconds value) noexcept {
        return static_cast<long>(value.count() / 1000);
    }
};
#else
    static bool initializeLib() noexcept { return true; }
    static void cleanupLib() noexcept {}

    static std::string getErrorText(bool prependErrorId = true) { return getErrorText(getLastError(), prependErrorId); }
    static std::string getErrorText(int error, bool prependErrorId = true) {
        auto errorText = strerror(error);
        std::string result(errorText ? errorText : "");
        if (prependErrorId) {
            result.insert(0, std::to_string(error) + ":");
        }
        return result;
    }


    static constexpr auto toTimeout(std::chrono::microseconds uSec) noexcept {
        timeval timeoutTimeValue{ static_cast<decltype(timeval::tv_sec)>(uSec.count() / 1000000u),
                                 static_cast<decltype(timeval::tv_usec)>(uSec.count() % 1000000u) };

        return timeoutTimeValue;
    }

protected:
    using SocketType = int;
    using SockLen = socklen_t;
    static const SocketType invalidSocket = -1;
    static const int socketError = -1;

    enum class Shutdown { Read = SHUT_RD, Write = SHUT_WR, ReadWrite = SHUT_RDWR };
    enum class Error { Timeout = EAGAIN, WouldBlock = EWOULDBLOCK };

    static bool closeSocket(SocketType socket) { return ::close(socket) != socketError; }
    static auto getLastError() { return errno; }
#endif
};

} // namespace

net::io_context ioContext;
net::ip::tcp::acceptor acceptor;
net::ip::tcp::resolver resolver(ioContext);
net::ip::tcp::endpoint endpoint = *resolver.resolve(address, port).begin();


net::ip::tcp::socket



class io_context {
};

namespace ip {
namespace tcp {

class acceptor {

};


class endpoint {

};


class resolver {

};

class socket {
    io_context& ioContext;
public:
    socket(io_context& context) : ioContext(context) {}

};

} // namespace tcp
} // namespace ip
} // namespace net
} // namespace webfront

*/