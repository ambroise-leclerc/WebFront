# Keep dependency settings local to this function: mddlog's package sets module defaults.
function(webfront_add_mddlog)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.40")
            message(FATAL_ERROR "MSVC compiler 19.40 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Match mddlog: GCC 16.1 is qualified; 16.2 corrupts module BMIs.
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1")
            message(FATAL_ERROR "GCC 16.1 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
        elseif(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "16.2" AND
               CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.3")
            message(FATAL_ERROR
                "GCC 16.2 is unsupported: it corrupts this project's module BMIs ('failed to read "
                "compiled module cluster ... Bad file data'). Use GCC 16.1 (CI pins gcc:16.1.0 "
                "exactly for the same reason). Current version: ${CMAKE_CXX_COMPILER_VERSION}")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "20.0")
            message(FATAL_ERROR "Clang 20 or later is required for C++23 modules support. Current version: ${CMAKE_CXX_COMPILER_VERSION}")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(FATAL_ERROR
            "AppleClang is unsupported. Apple Silicon macOS requires upstream LLVM/Clang 21.1.8; "
            "select the upstream LLVM toolchain described in docs/mddlog-integration.md.")
    else()
        message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. mddlog requires MSVC compiler 19.40+, GCC 16.1+, or upstream Clang 20+ for C++23 modules support.")
    endif()

    if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "Unsupported Apple target '${CMAKE_SYSTEM_NAME}'. mddlog supports Apple Silicon macOS only.")
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
            message(FATAL_ERROR "mddlog supports macOS only on Apple Silicon (arm64).")
        endif()
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "mddlog on macOS requires upstream LLVM/Clang 21.1.8; AppleClang and GCC are unsupported.")
        endif()
        if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "21.1.8")
            message(FATAL_ERROR
                "mddlog's verified macOS compiler is upstream Clang 21.1.8 exactly; found "
                "${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
        if(NOT CMAKE_VERSION VERSION_EQUAL "4.3.1")
            message(FATAL_ERROR "mddlog's verified macOS CMake is 4.3.1 exactly; found ${CMAKE_VERSION}.")
        endif()
        function(_mddlog_verify_llvm_archive_tool tool_path expected_name)
            get_filename_component(tool_name "${tool_path}" NAME)
            if(NOT tool_name STREQUAL expected_name)
                message(FATAL_ERROR
                    "The macOS build requires ${expected_name}, but CMake selected '${tool_path}'. "
                    "Select the upstream LLVM toolchain described in docs/mddlog-integration.md.")
            endif()

            execute_process(
                COMMAND "${tool_path}" --version
                RESULT_VARIABLE tool_result
                OUTPUT_VARIABLE tool_stdout
                ERROR_VARIABLE tool_stderr
            )
            string(CONCAT tool_banner "${tool_stdout}" "\n" "${tool_stderr}")
            if(NOT tool_result EQUAL 0 OR
               NOT tool_banner MATCHES "LLVM version 21\\.1\\.8([^0-9.]|$)")
                message(FATAL_ERROR
                    "The macOS build requires LLVM 21.1.8 ${expected_name}; '${tool_path} --version' "
                    "returned '${tool_banner}'.")
            endif()
        endfunction()

        _mddlog_verify_llvm_archive_tool("${CMAKE_AR}" "llvm-ar")
        _mddlog_verify_llvm_archive_tool("${CMAKE_RANLIB}" "llvm-ranlib")
    endif()

    # Checked after the compiler/platform diagnostics so unsupported combinations get the specific
    # remediation above rather than a generic missing-import-metadata error.
    if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
        message(STATUS "C++23 import std metadata available")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.40")
        message(STATUS "__CMAKE::CXX23 target NOT available, but MSVC ${CMAKE_CXX_COMPILER_VERSION} should support import std with /experimental:module")
    else()
        message(FATAL_ERROR
            "The selected compiler and standard library do not provide C++23 import std support. "
            "CMAKE_CXX_COMPILER_IMPORT_STD='${CMAKE_CXX_COMPILER_IMPORT_STD}'.")
    endif()

    if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "mddlog supports Windows, Linux and Apple Silicon macOS only.")
    endif()
    set(WEBFRONT_MDDLOG_SOURCE_DIR "" CACHE PATH "Optional mddlog source tree; otherwise use find_package")
    if(WEBFRONT_MDDLOG_SOURCE_DIR)
        # Only the module libraries belong in the WebFront dependency graph.
        # mddlog has modules with the same names as WebFront's build helpers.
        # Resolve its helpers first, in this function's scope only.
        list(PREPEND CMAKE_MODULE_PATH "${WEBFRONT_MDDLOG_SOURCE_DIR}/cmake")
        set(MDDLOG_BUILD_TESTS OFF)
        set(MDDLOG_BUILD_EXAMPLES OFF)
        add_subdirectory("${WEBFRONT_MDDLOG_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mddlog")
    elseif(NOT TARGET mddlog::mddlog)
        find_package(mddlog CONFIG REQUIRED)
    endif()

    add_library(WebFront_mddlog STATIC "${PROJECT_SOURCE_DIR}/logging/Logger.cpp")
    add_library(WebFront::mddlog ALIAS WebFront_mddlog)
    target_compile_features(WebFront_mddlog PUBLIC cxx_std_23)
    set_target_properties(WebFront_mddlog PROPERTIES CXX_SCAN_FOR_MODULES ON)
    if(23 IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
        set_target_properties(WebFront_mddlog PROPERTIES CXX_MODULE_STD ON)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(WebFront_mddlog PRIVATE /experimental:module /std:c++latest)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(WebFront_mddlog PRIVATE -fmodules)
    endif()
    target_include_directories(WebFront_mddlog PUBLIC "${PROJECT_SOURCE_DIR}/include")
    # The TU directly imports core modules; link their provider explicitly for GCC's mapper.
    target_link_libraries(WebFront_mddlog PRIVATE mddlog::mddlog mddlog::core)
    target_compile_definitions(WebFront_mddlog PUBLIC WEBFRONT_USE_MDDLOG=1)
    target_link_libraries(WebFront INTERFACE WebFront_mddlog)
endfunction()
