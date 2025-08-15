#pragma once

// Windows compatibility header - centralizes Windows-specific macro definitions
// Include this header before any Windows-specific includes to ensure consistent configuration

#ifdef _WIN32

// Prevent Windows min/max macros from interfering with std::min/max
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Reduce Windows header bloat and compilation time
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Ensure WinSock2.h is included before any headers that might include WinSock.h
// This prevents conflicts between WinSock and WinSock2 definitions
#include <winsock2.h>
#include <ws2tcpip.h>

// Common Windows headers that benefit from the above macros
#include <windows.h>

#endif // _WIN32