# Exercise the CEF architecture selection without downloading or configuring the SDK.
set(UNIX ON)

function(check_project_arch processor expected)
    set(CMAKE_SYSTEM_PROCESSOR "${processor}")
    unset(PROJECT_ARCH CACHE)
    unset(PROJECT_ARCH)
    include("${ROOT}/cmake/CEFArchitecture.cmake")
    if(expected STREQUAL "unset")
        if(DEFINED PROJECT_ARCH)
            message(FATAL_ERROR "Processor ${processor}: PROJECT_ARCH must stay unset, got ${PROJECT_ARCH}")
        endif()
    elseif(NOT PROJECT_ARCH STREQUAL expected)
        message(FATAL_ERROR "Processor ${processor}: expected PROJECT_ARCH=${expected}, got '${PROJECT_ARCH}'")
    endif()
endfunction()

# Linux aarch64 and arm64 must never reach CEF's x86_64 branch (-m64 -march=x86-64).
check_project_arch(aarch64 arm64)
check_project_arch(arm64 arm64)
# Unchanged: x86_64 stays on the SDK's detection.
check_project_arch(x86_64 unset)
check_project_arch(AMD64 unset)

# A value given by the user is never overwritten, even for a cross-compiled arm64 target.
set(CMAKE_SYSTEM_PROCESSOR aarch64)
unset(PROJECT_ARCH CACHE)
set(PROJECT_ARCH x86_64)
include("${ROOT}/cmake/CEFArchitecture.cmake")
if(NOT PROJECT_ARCH STREQUAL "x86_64")
    message(FATAL_ERROR "A user-provided PROJECT_ARCH was overwritten: ${PROJECT_ARCH}")
endif()

# The target decides, not the host: an x86_64 host cross-compiling for aarch64 gets arm64.
set(CMAKE_HOST_SYSTEM_PROCESSOR x86_64)
check_project_arch(aarch64 arm64)

# macOS and Windows keep the SDK's own detection.
unset(UNIX)
check_project_arch(aarch64 unset)
set(UNIX ON)
set(APPLE ON)
check_project_arch(arm64 unset)
