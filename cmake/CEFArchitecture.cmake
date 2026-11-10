# CEF's cef_variables.cmake recognizes ARM64 only through the macOS host name ("arm64") or MSVC, so
# Linux "aarch64" falls through to x86_64 and the wrapper is built with -m64 -march=x86-64.
# Select arm64 from the *target* processor before the SDK is included, and never override a
# PROJECT_ARCH given by the user. Apple and Windows keep the SDK's own detection.
#
# The CEF package picks its Linux archive with `uname -m`, i.e. from the host, and offers no way to
# override it. A Linux build whose target architecture differs from the host's would therefore
# compile the wrapper for one architecture and link a libcef.so of the other, so it is refused here
# until the package selects the archive from the target.
if(UNIX AND NOT APPLE)
    set(_webfront_cef_arm64 "^(aarch64|arm64|ARM64)$")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "${_webfront_cef_arm64}")
        set(_webfront_cef_target_arm64 TRUE)
    else()
        set(_webfront_cef_target_arm64 FALSE)
    endif()
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "${_webfront_cef_arm64}")
        set(_webfront_cef_host_arm64 TRUE)
    else()
        set(_webfront_cef_host_arm64 FALSE)
    endif()
    if(NOT _webfront_cef_target_arm64 STREQUAL _webfront_cef_host_arm64)
        message(FATAL_ERROR
            "WEBFRONT_EMBED_CEF does not support cross-compiling from ${CMAKE_HOST_SYSTEM_PROCESSOR} to "
            "${CMAKE_SYSTEM_PROCESSOR}: the CEF package downloads the archive of the host architecture. "
            "Build on the target architecture, or use WEBFRONT_EMBED_CEF=OFF.")
    endif()
    if(_webfront_cef_target_arm64 AND NOT DEFINED PROJECT_ARCH)
        set(PROJECT_ARCH "arm64" CACHE STRING "CEF target architecture (set by WebFront for Linux aarch64)")
        message(STATUS "WebFront: CEF target is Linux ${CMAKE_SYSTEM_PROCESSOR}, using PROJECT_ARCH=arm64")
    endif()
    unset(_webfront_cef_arm64)
    unset(_webfront_cef_target_arm64)
    unset(_webfront_cef_host_arm64)
endif()
