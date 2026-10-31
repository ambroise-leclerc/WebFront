/// @brief Shared native declarations: WinSock must precede Windows for every automation include order.
#pragma once
#include <cerrno>
#ifdef _WIN32
    // WinSock must precede Windows, and BCrypt requires Windows' type declarations.
    #ifndef NOMINMAX
        #define NOMINMAX
        #define WEBFRONT_AUTOMATION_PLATFORM_RESTORE_NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
        #define WEBFRONT_AUTOMATION_PLATFORM_RESTORE_LEAN
    #endif
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <sddl.h>
    // clang-format on
    #ifdef WEBFRONT_AUTOMATION_PLATFORM_RESTORE_LEAN
        #undef WEBFRONT_AUTOMATION_PLATFORM_RESTORE_LEAN
        #undef WIN32_LEAN_AND_MEAN
    #endif
    #ifdef WEBFRONT_AUTOMATION_PLATFORM_RESTORE_NOMINMAX
        #undef WEBFRONT_AUTOMATION_PLATFORM_RESTORE_NOMINMAX
        #undef NOMINMAX
    #endif
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #if defined(__APPLE__) || defined(__linux__)
        #include <sys/random.h>
    #endif
#endif

#ifndef _WIN32
namespace webfront::automation::platform {
inline constexpr bool retryablePosixError(int code) {
    if (code == EINTR || code == EAGAIN)
        return true;
    #if EWOULDBLOCK != EAGAIN
    if (code == EWOULDBLOCK)
        return true;
    #endif
    return false;
}
}  // namespace webfront::automation::platform
#endif
