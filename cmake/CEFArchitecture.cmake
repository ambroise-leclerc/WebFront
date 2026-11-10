# CEF's cef_variables.cmake recognizes ARM64 only through the macOS host name ("arm64") or MSVC, so
# Linux "aarch64" falls through to x86_64 and the wrapper is built with -m64 -march=x86-64.
# Select arm64 from the *target* processor before the SDK is included, and never override a
# PROJECT_ARCH given by the user. Apple and Windows keep the SDK's own detection.
#
# The CEF package picks its Linux archive with `uname -m`, i.e. from the host, and offers no way to
# override it. A Linux build whose target architecture differs from the host's would therefore
# compile the wrapper for one architecture and link a libcef.so of the other, so it is refused here
# until the package selects the archive from the target.
function(_webfront_cef_normalize_architecture processor result)
    string(TOLOWER "${processor}" processor)
    if(processor MATCHES "^(aarch64|arm64)$")
        set(${result} arm64 PARENT_SCOPE)
    elseif(processor MATCHES "^(x86_64|amd64|x64)$")
        set(${result} x86_64 PARENT_SCOPE)
    elseif(processor MATCHES "^i[3-6]86$")
        set(${result} x86 PARENT_SCOPE)
    else()
        set(${result} "${processor}" PARENT_SCOPE)
    endif()
endfunction()

if(UNIX AND NOT APPLE)
    _webfront_cef_normalize_architecture("${CMAKE_SYSTEM_PROCESSOR}" _webfront_cef_target)
    _webfront_cef_normalize_architecture("${CMAKE_HOST_SYSTEM_PROCESSOR}" _webfront_cef_host)
    if(NOT _webfront_cef_target STREQUAL _webfront_cef_host)
        message(FATAL_ERROR
            "WEBFRONT_EMBED_CEF does not support cross-compiling from ${CMAKE_HOST_SYSTEM_PROCESSOR} to "
            "${CMAKE_SYSTEM_PROCESSOR}: the CEF package downloads the archive of the host architecture. "
            "Build on the target architecture, or use WEBFRONT_EMBED_CEF=OFF.")
    endif()
    if(_webfront_cef_target STREQUAL "arm64" AND NOT DEFINED PROJECT_ARCH)
        set(PROJECT_ARCH "arm64" CACHE STRING "CEF target architecture (set by WebFront for Linux aarch64)")
        message(STATUS "WebFront: CEF target is Linux ${CMAKE_SYSTEM_PROCESSOR}, using PROJECT_ARCH=arm64")
    endif()
    unset(_webfront_cef_target)
    unset(_webfront_cef_host)
endif()
