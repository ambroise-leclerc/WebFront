/// @date   13/08/2015
/// @author Ambroise Leclerc
///
/// OS abstration layers for socket libraries.
///
#pragma once
#include <array>
#include <chrono>
#include <string>
#include <tuple>


#ifdef WIN32
#include <IPHlpApi.h>
#include <IcmpAPI.h>
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace webfront {
namespace net {

class SocketLibOS {
public:
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

    static std::tuple<uint8_t, std::chrono::microseconds> ping(uint32_t address) noexcept {
        auto icmpFile = IcmpCreateFile();

        if (icmpFile == INVALID_HANDLE_VALUE) return std::make_tuple(uint8_t(0), std::chrono::milliseconds{0});

        std::array<uint8_t, 32> sendBuffer{"Data Buffer"};
        std::array<uint8_t, sizeof(ICMP_ECHO_REPLY) + sendBuffer.size()> replyBuffer;
        auto replies =
          static_cast<uint8_t>(IcmpSendEcho(icmpFile, address, sendBuffer.data(), static_cast<WORD>(sendBuffer.size()), nullptr,
                                            replyBuffer.data(), static_cast<DWORD>(replyBuffer.size()), 1000));
        IcmpCloseHandle(icmpFile);
        return std::make_tuple(
          replies, std::chrono::microseconds(reinterpret_cast<PICMP_ECHO_REPLY>(replyBuffer.data())->RoundTripTime * 1000));
    }

    static constexpr auto toTimeout(const std::chrono::microseconds value) noexcept {
        return static_cast<long>(value.count() / 1000);
    }
};
#else
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <regex>
#include <string>

namespace webfront {
namespace net {

class SocketLibOS {
protected:
    static auto getLastError() { return errno; }

public:
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

    static std::tuple<uint8_t, std::chrono::microseconds> ping(uint32_t address) noexcept {
        using namespace std;
        using namespace string_literals;

        char addrStrBuffer[INET_ADDRSTRLEN];
        string addressText = inet_ntop(AF_INET, &address, addrStrBuffer, INET_ADDRSTRLEN);
        auto command = ::popen(("/bin/ping -c 1 "s + addressText).c_str(), "r");
        if (command == nullptr) return make_tuple(0, std::chrono::microseconds{0});

        auto nbReplies = 0;
        auto tripTime = .0;
        regex replyRegex(".*time=\\s*([0-9]+(\\.[0-9]+)?)\\s*ms.*\n*$");
        cmatch timeResult;
        array<char, 80> reply;

        while (::fgets(reply.data(), reply.size() - 1, command) != nullptr) {
            if (regex_match(reply.data(), timeResult, replyRegex)) {
                nbReplies++;
                tripTime += stod(timeResult.str(1));
            }
        }
        if (nbReplies == 0) return make_tuple(0, std::chrono::microseconds{0});

        return make_tuple(nbReplies, std::chrono::microseconds{uint32_t(1000. * tripTime / nbReplies)});
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
};

#endif

} // namespace net
} // namespace webfront
