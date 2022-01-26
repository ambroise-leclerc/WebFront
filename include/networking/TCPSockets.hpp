/// @date 26/01/2022 11:38:19
/// @author Ambroise Leclerc
/// @brief A BSD socket/ winsock implementation 
#pragma once
#include "BasicNetworking.hpp"

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
namespace networking {

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
        auto bufLen = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                                    reinterpret_cast<LPTSTR>(&lpMsgBuf), 0, nullptr);
        if (bufLen) {
            auto lpMsgStr = reinterpret_cast<LPCSTR>(lpMsgBuf);
            std::string result(lpMsgStr, lpMsgStr + bufLen);
            LocalFree(lpMsgBuf);
            if (prependErrorId) { result.insert(0, std::to_string(error) + ":"); }
            return result;
        }
        return std::string();
    }

    static constexpr auto toTimeout(const std::chrono::microseconds value) noexcept { return static_cast<long>(value.count() / 1000); }
};
#else
    static bool initializeLib() noexcept { return true; }
    static void cleanupLib() noexcept {}

    static std::string getErrorText(bool prependErrorId = true) { return getErrorText(getLastError(), prependErrorId); }
    static std::string getErrorText(int error, bool prependErrorId = true) {
        auto errorText = strerror(error);
        std::string result(errorText ? errorText : "");
        if (prependErrorId) { result.insert(0, std::to_string(error) + ":"); }
        return result;
    }

    static constexpr auto toTimeout(std::chrono::microseconds uSec) noexcept {
        timeval timeoutTimeValue{static_cast<decltype(timeval::tv_sec)>(uSec.count() / 1000000u),
                                 static_cast<decltype(timeval::tv_usec)>(uSec.count() % 1000000u)};

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


class TCPSockets : public BasicNetworking<> {
public:
    using super::ConstBuffer;
    using super::MutableBuffer;

};

} // namespace networking
} // namespace webfront