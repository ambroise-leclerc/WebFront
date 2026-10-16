# Exercise the production SDK selection without downloading or linking the SDK.
set(WIN32 ON)
set(MSVC ON)
function(CPMAddPackage)
endfunction()
function(target_include_directories)
endfunction()
function(target_compile_definitions)
endfunction()
macro(target_link_libraries)
    set(linked_libraries "${ARGN}")
endmacro()

function(check_architecture compiler host platform pointer_size expected)
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "${compiler}")
    set(CMAKE_SYSTEM_PROCESSOR "${host}")
    set(CMAKE_GENERATOR_PLATFORM "${platform}")
    set(CMAKE_SIZEOF_VOID_P "${pointer_size}")
    include("${ROOT}/cmake/WebView2Support.cmake")
    if(NOT linked_libraries MATCHES "/native/${expected}/WebView2LoaderStatic[.]lib")
        message(FATAL_ERROR "Compiler ${compiler}, host ${host}, platform '${platform}': expected ${expected}, got ${linked_libraries}")
    endif()
endfunction()

if(DEFINED UNSUPPORTED_TARGET)
    check_architecture("${UNSUPPORTED_TARGET}" ARM64 "" 8 unused)
    message(FATAL_ERROR "Unsupported compiler target was accepted")
endif()

# Ninja has no generator platform. A native ARM64 host can target x64 or x86.
check_architecture(x64 ARM64 "" 8 x64)
check_architecture(X86 ARM64 "" 4 x86)
check_architecture(ARM64 AMD64 "" 8 arm64)
check_architecture(x64 ARM64 x64 8 x64)
check_architecture(X86 AMD64 Win32 4 x86)
check_architecture(ARM64 ARM64 ARM64 8 arm64)

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DROOT=${ROOT}" -DUNSUPPORTED_TARGET=ARM64EC -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "does not support compiler target 'ARM64EC'")
    message(FATAL_ERROR "An unsupported SDK architecture must fail explicitly: ${output}${error}")
endif()
