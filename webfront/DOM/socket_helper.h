#ifndef __SOCKET_HELPER_H_
#define __SOCKET_HELPER_H_

#ifdef WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SOCKET SocketType;
class SocketHelper
{
public:
    static const SocketType kInvalidSocket = INVALID_SOCKET;

    static bool InitializeLib()
    {
        WSADATA wsa_data;
        int init_value = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        return init_value == 0;
    }

    static void CleanupLib() { WSACleanup(); }
    static void CloseSocket(SocketType socket) { closesocket(socket); }
    static void Shutdown(SocketType socket) { shutdown(socket, SD_BOTH); }
};
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

typedef int SocketType;
class SocketHelper
{
public:
    static const SocketType kInvalidSocket = -1;

    static bool InitializeLib() { return true; }
    static void CleanupLib() {}
    static void CloseSocket(SocketType socket) { close(socket); }
    static void Shutdown(SocketType socket) { shutdown(socket, SHUT_RDWR); }
};

#endif
#endif //__SOCKET_HELPER_H_