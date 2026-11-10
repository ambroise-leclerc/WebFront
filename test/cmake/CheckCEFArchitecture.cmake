# Exercise the CEF architecture selection without downloading or configuring the SDK. Every case sets
# UNIX, APPLE and both processors explicitly: this script may itself run on macOS or on any host.

function(check_project_arch host target expected)
    set(UNIX ON)
    set(APPLE OFF)
    set(CMAKE_HOST_SYSTEM_PROCESSOR "${host}")
    set(CMAKE_SYSTEM_PROCESSOR "${target}")
    unset(PROJECT_ARCH CACHE)
    unset(PROJECT_ARCH)
    include("${ROOT}/cmake/CEFArchitecture.cmake")
    if(expected STREQUAL "unset")
        if(DEFINED PROJECT_ARCH)
            message(FATAL_ERROR "${host} -> ${target}: PROJECT_ARCH must stay unset, got ${PROJECT_ARCH}")
        endif()
    elseif(NOT PROJECT_ARCH STREQUAL expected)
        message(FATAL_ERROR "${host} -> ${target}: expected PROJECT_ARCH=${expected}, got '${PROJECT_ARCH}'")
    endif()
endfunction()

# Cross-compilation cases run in a child process: the module must fail, with the explanation.
if(DEFINED CROSS_HOST)
    check_project_arch("${CROSS_HOST}" "${CROSS_TARGET}" unset)
    message(FATAL_ERROR "Cross-compilation ${CROSS_HOST} -> ${CROSS_TARGET} was accepted")
endif()

# Native builds. Linux aarch64 and arm64 must never reach CEF's x86_64 branch (-m64 -march=x86-64).
check_project_arch(aarch64 aarch64 arm64)
check_project_arch(arm64 arm64 arm64)
# x86_64 stays on the SDK's detection.
check_project_arch(x86_64 x86_64 unset)
check_project_arch(AMD64 AMD64 unset)

# A value given by the user is never overwritten.
set(UNIX ON)
set(APPLE OFF)
set(CMAKE_HOST_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
unset(PROJECT_ARCH CACHE)
set(PROJECT_ARCH x86_64)
include("${ROOT}/cmake/CEFArchitecture.cmake")
if(NOT PROJECT_ARCH STREQUAL "x86_64")
    message(FATAL_ERROR "A user-provided PROJECT_ARCH was overwritten: ${PROJECT_ARCH}")
endif()

# The package downloads the archive of the host: both directions of a cross-build are refused,
# instead of compiling the wrapper for one architecture and linking libcef.so of the other.
foreach(pair "x86_64;aarch64" "aarch64;x86_64")
    list(GET pair 0 host)
    list(GET pair 1 target)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DROOT=${ROOT}" "-DCROSS_HOST=${host}" "-DCROSS_TARGET=${target}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "does not support cross-compiling from ${host} to ${target}")
        message(FATAL_ERROR "Cross-compilation ${host} -> ${target} must fail explicitly: ${output}${error}")
    endif()
endforeach()

# macOS and Windows keep the SDK's own detection, whatever the processors.
set(CMAKE_HOST_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
unset(PROJECT_ARCH CACHE)
unset(PROJECT_ARCH)
set(UNIX OFF)
set(APPLE OFF)
include("${ROOT}/cmake/CEFArchitecture.cmake")
set(UNIX ON)
set(APPLE ON)
include("${ROOT}/cmake/CEFArchitecture.cmake")
if(DEFINED PROJECT_ARCH)
    message(FATAL_ERROR "Windows and macOS must keep the SDK's detection, got PROJECT_ARCH=${PROJECT_ARCH}")
endif()
