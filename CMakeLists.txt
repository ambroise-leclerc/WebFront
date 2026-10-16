cmake_minimum_required (VERSION 3.31)

# Top-level detection must precede project(): an embedding parent project owns global settings.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WEBFRONT_IS_TOP_LEVEL ON)
else()
    set(WEBFRONT_IS_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Enable compilation of shared libraries" OFF)
option(ENABLE_TESTING "Enable Test Builds" OFF)
option(WEBFRONT_BUILD_EXAMPLES "Build the example applications" ${WEBFRONT_IS_TOP_LEVEL})
option(ENABLE_COVERAGE "Enable test coverage instrumentation" OFF)
option(ENABLE_FUZZING "Enable Fuzzing Builds" OFF)
option(WEBFRONT_USE_MDDLOG "Use the compiled mddlog diagnostic adapter" OFF)
# The experimental gate must be selected before project() probes the compiler.
if(WEBFRONT_USE_MDDLOG)
    include(cmake/MddlogPreProject.cmake)
endif()

option(WEBFRONT_EMBED_CEF "Enable embedded CEF window support" OFF)
option(WEBFRONT_EMBED_WEBVIEW2 "Enable embedded WebView2 window support (Windows only)" OFF)

set(VERSION_STRING_REGEX "inline constexpr std::string_view version = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/WebFront.hpp" VERSION_STRING REGEX ${VERSION_STRING_REGEX})
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

# The library requires threads; consumers inherit the flag through the WebFront target.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(WebFront INTERFACE)
add_library(WebFront::WebFront ALIAS WebFront)
target_link_libraries(WebFront INTERFACE Threads::Threads)

# Static linkage for CEF compatibility (a global setting: only the top-level project may choose it)
if(MSVC AND NOT WEBFRONT_USE_MDDLOG AND WEBFRONT_IS_TOP_LEVEL)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
if(WEBFRONT_IS_TOP_LEVEL)
    include(StandardProjectSettings)
    include(PreventInSourceBuilds)
endif()
add_library(WebFront_options INTERFACE)
target_compile_features(WebFront_options INTERFACE cxx_std_23)
# Do not force-define __cpp_lib_format on the command line: unlike a header setting it after
# actually providing <format>, a command-line -D applies from the start of every translation
# unit, so other standard headers (e.g. <vector>) that gate their own <format> integration on
# this macro see it "on" before <format> has been included and fail to compile. Let each
# toolchain's own <version>/<format> define it accurately; include/details/C++20Support.hpp
# already falls back to a minimal std::format polyfill when a toolchain leaves it undefined.

add_library(WebFront_warnings INTERFACE)

include(Cache)

include(Linker)
configure_linker(WebFront_options)

include(CompilerWarnings)
set_project_warnings(WebFront_warnings)

include(Sanitizers)
enable_sanitizers(WebFront_options)

if(WEBFRONT_IS_TOP_LEVEL)
    configure_file(Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile)
    include(Doxygen)
    enable_doxygen()

    include(StaticAnalyzers)
endif()
include(CPM)
if(WEBFRONT_EMBED_WEBVIEW2)
    include(WebView2Support)
endif()

# Define the APPEND_PLATFORM_SOURCES macro needed by CEF
macro(APPEND_PLATFORM_SOURCES variable_name)
  if(WIN32 AND DEFINED ${variable_name}_WINDOWS)
    list(APPEND ${variable_name} ${${variable_name}_WINDOWS})
  endif()
  if(UNIX AND NOT APPLE AND DEFINED ${variable_name}_LINUX)
    list(APPEND ${variable_name} ${${variable_name}_LINUX})
  endif()
  if(APPLE AND DEFINED ${variable_name}_MAC)
    list(APPEND ${variable_name} ${${variable_name}_MAC})
  endif()
endmacro()

# Add the ADD_LOGICAL_TARGET macro for macOS compatibility
macro(ADD_LOGICAL_TARGET target)
  if(TARGET ${target})
    # Target already exists
  else()
    add_custom_target(${target})
  endif()
endmacro()

CPMAddPackage("gh:ambroise-leclerc/stdnet#8005774eabcba7e73188c02d2eeca2ff43c4ee3a")

# Only build CEF if embedded window support is enabled
if(WEBFRONT_EMBED_CEF)
    message(STATUS "WebFront: Embedded CEF window support enabled")
    
    # Configure CEF build options to avoid binary distribution issues
    set(CEF_USE_SANDBOX OFF CACHE BOOL "Disable CEF sandbox" FORCE)
    set(CEF_WRAPPER_BUILD_SKIP OFF CACHE BOOL "Enable CEF wrapper build" FORCE)

    # Completely disable CEF tests to avoid linking issues on macOS
    set(CEF_SKIP_TESTS ON CACHE BOOL "Skip CEF tests" FORCE)
    set(CEF_BUILD_TESTS OFF CACHE BOOL "Disable CEF test building" FORCE)
    set(BUILD_CEF_TESTS OFF CACHE BOOL "Disable CEF test building" FORCE)
    set(CEF_BUILD_LIBCEF_DLL_WRAPPER ON CACHE BOOL "Enable CEF wrapper" FORCE)

    # Prevent CEF from adding test targets
    set(OPTION_BUILD_TESTS OFF CACHE BOOL "Disable all test building" FORCE)
    set(BUILD_TESTING_SAVED ${BUILD_TESTING})
    set(BUILD_TESTING OFF CACHE BOOL "Disable testing during CEF build" FORCE)

    # Additional aggressive test prevention for CEF
    set(CEF_ENABLE_TESTS OFF CACHE BOOL "Disable CEF tests" FORCE)
    set(ENABLE_CEF_TESTS OFF CACHE BOOL "Disable CEF tests" FORCE)
    set(WITH_TESTS OFF CACHE BOOL "Disable all tests" FORCE)

    CPMAddPackage("gh:ambroise-leclerc/CEF@154.0.34")
else()
    message(STATUS "WebFront: Embedded CEF window support disabled")
endif()

# Workaround: CEF ships only release libs (iterator debug level 0). Prevent MSVC debug iterator level
# mismatch; the definitions propagate to every target linking WebFront (examples, tests, consumers).
if (WEBFRONT_EMBED_CEF AND MSVC)
    target_compile_definitions(WebFront INTERFACE _ITERATOR_DEBUG_LEVEL=0 _HAS_ITERATOR_DEBUGGING=0)
endif()

# Override CEF testing to prevent any test building on macOS and problematic platforms
if(WEBFRONT_EMBED_CEF AND (APPLE OR CMAKE_SYSTEM_NAME MATCHES "Darwin"))
    # Replace the CEF testing file to disable all tests
    file(WRITE "${CMAKE_BINARY_DIR}/_deps/cef-src/cmake/CEFTesting.cmake"
        "# CEF Testing disabled for macOS compatibility\n"
        "# Original file replaced to prevent test building issues\n"
        "message(STATUS \"CEF testing has been disabled for platform compatibility\")\n"
        "# Do not enable testing or add test subdirectory\n"
    )
    message(STATUS "Disabled CEF testing for macOS compatibility")
endif()

# Restore the original BUILD_TESTING setting after CEF is configured
set(BUILD_TESTING ${BUILD_TESTING_SAVED} CACHE BOOL "Restore original testing setting" FORCE)

# Handle macOS CEF framework structure
if(APPLE AND EXISTS "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src")
    # Check if the libcef_dll_wrapper directory exists, create it if not
    set(CEF_WRAPPER_DIR "${CMAKE_BINARY_DIR}/_deps/cef_binaries-src/libcef_dll")
    if(NOT EXISTS "${CEF_WRAPPER_DIR}")
        message(STATUS "Creating libcef_dll directory for macOS compatibility")
        file(MAKE_DIRECTORY "${CEF_WRAPPER_DIR}")
        
        # Create a minimal CMakeLists.txt for compatibility
        file(WRITE "${CEF_WRAPPER_DIR}/CMakeLists.txt"
            "# Minimal libcef_dll_wrapper CMakeLists.txt for macOS compatibility\n"
            "cmake_minimum_required(VERSION 3.31)\n"
            "project(libcef_dll_wrapper)\n"
            "\n"
            "# Create an empty interface target for compatibility\n"
            "add_library(libcef_dll_wrapper INTERFACE)\n"
            "target_include_directories(libcef_dll_wrapper INTERFACE \${CMAKE_CURRENT_SOURCE_DIR})\n"
        )
        message(STATUS "Created minimal libcef_dll_wrapper for macOS")
    endif()
endif()

if(WEBFRONT_USE_MDDLOG)
    include(cmake/MddlogIntegration.cmake)
    webfront_add_mddlog()
endif()

# Warnings, sanitizers, coverage and linker choices (WebFront_warnings, WebFront_options) are opt-in per
# internal target, so they never leak into consuming projects; consumers only inherit the C++ standard.
target_compile_features(WebFront INTERFACE cxx_std_23)
target_link_libraries(WebFront INTERFACE stdnet::stdnet)
target_include_directories(WebFront INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Ajouter les headers CEF à WebFront comme headers système pour éviter les warnings
if(EXISTS "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}/include")
endif()

# Only define Windows version macros when building on Windows.
# Defining _WIN32_WINNT on non-Windows (e.g. WSL/Linux) breaks libstdc++ headers.
if (WIN32)
  # Prefer Windows 10 (0x0A00). Allow user override by predefining WINVER/_WIN32_WINNT.
  if (NOT DEFINED WINVER AND NOT DEFINED _WIN32_WINNT)
    target_compile_definitions(WebFront INTERFACE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
  endif()
endif()

# Embedded asset regeneration target and CTest freshness checks.
include(WebFrontAssets)
webfront_add_embedded_assets()

if(ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test)
  add_subdirectory(webtest)
endif()

if(WEBFRONT_BUILD_EXAMPLES)
  add_subdirectory(src)
endif()
