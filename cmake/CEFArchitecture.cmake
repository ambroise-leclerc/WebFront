# CEF's cef_variables.cmake recognizes ARM64 only through the macOS host name ("arm64") or MSVC, so
# Linux "aarch64" falls through to x86_64 and the wrapper is built with -m64 -march=x86-64.
# Select arm64 from the *target* processor before the SDK is included, and never override a
# PROJECT_ARCH given by the user. Apple and Windows keep the SDK's own detection.
if(UNIX AND NOT APPLE AND NOT DEFINED PROJECT_ARCH
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(PROJECT_ARCH "arm64" CACHE STRING "CEF target architecture (set by WebFront for Linux aarch64)")
    message(STATUS "WebFront: CEF target is Linux ${CMAKE_SYSTEM_PROCESSOR}, using PROJECT_ARCH=arm64")
endif()
