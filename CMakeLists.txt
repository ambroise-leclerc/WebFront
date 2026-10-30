# The embedding project's CMP0155 setting (module scanning by default), read before this file's own
# cmake_minimum_required resets the policies; empty when unknown or set.
cmake_policy(GET CMP0155 WEBFRONT_PARENT_CMP0155)
cmake_minimum_required (VERSION 3.31)

# Top-level detection must precede project(): an embedding parent project owns global settings.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WEBFRONT_IS_TOP_LEVEL ON)
else()
    set(WEBFRONT_IS_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Enable compilation of shared libraries" OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/WebFrontTestOptions.cmake)
webfront_declare_test_options(${WEBFRONT_IS_TOP_LEVEL})
option(WEBFRONT_BUILD_AUTOMATION "Build the optional local developer CLI/MCP launcher" OFF)
option(WEBFRONT_BUILD_EXAMPLES "Build the example applications" ${WEBFRONT_IS_TOP_LEVEL})
option(WEBFRONT_GZIP_DIFFERENTIAL "Build test-only gzip/zlib differential checks" OFF)
option(WEBFRONT_USE_MDDLOG "Use the compiled mddlog diagnostic adapter" OFF)
# The experimental gate must be selected before project() probes the compiler.
if(WEBFRONT_USE_MDDLOG)
    include(cmake/MddlogPreProject.cmake)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/webfront/WebFront.hpp")
set(VERSION_STRING_REGEX "inline constexpr std::string_view version = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/webfront/WebFront.hpp" VERSION_STRING REGEX ${VERSION_STRING_REGEX})
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

include(cmake/Frontends.cmake)
# Applications need no -DWEBFRONT_EMBED_* option: unless the user decided, this platform's native window
# engine is enabled when its dependencies are present. Test and fuzzing builds keep explicit selection.
if(NOT WEBFRONT_ENABLE_TESTING AND NOT WEBFRONT_ENABLE_FUZZING)
    set(WEBFRONT_AUTODETECT_NATIVE_DEFAULT ON)
else()
    set(WEBFRONT_AUTODETECT_NATIVE_DEFAULT OFF)
endif()
option(WEBFRONT_AUTODETECT_NATIVE "Enable the native window engine of this platform when its dependencies are found" ${WEBFRONT_AUTODETECT_NATIVE_DEFAULT})
if(WEBFRONT_AUTODETECT_NATIVE)
    webfront_autodetect_native_frontends()
endif()
webfront_declare_frontend_options()

# The library requires threads; consumers inherit the flag through the WebFront target.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(WebFront INTERFACE)
add_library(WebFront::WebFront ALIAS WebFront)
target_link_libraries(WebFront INTERFACE Threads::Threads)
if(WIN32)
    target_link_libraries(WebFront INTERFACE shell32)
endif()

# Static linkage for CEF compatibility (a global setting: only the top-level project may choose it)
if(MSVC AND NOT WEBFRONT_USE_MDDLOG AND WEBFRONT_IS_TOP_LEVEL)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake ${CMAKE_CURRENT_LIST_DIR}/js/core ${CMAKE_CURRENT_LIST_DIR}/test/cmake)
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
# toolchain's own <version>/<format> define it accurately; include/webfront/details/C++20Support.hpp
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
    include(Doxygen)
    enable_doxygen()

    include(StaticAnalyzers)
endif()
include(CPM)
CPMAddPackage("gh:ambroise-leclerc/stdnet#8005774eabcba7e73188c02d2eeca2ff43c4ee3a")
webfront_include_frontend_support()

include(ModuleScan)
if(NOT WEBFRONT_IS_TOP_LEVEL)
    webfront_check_module_scan()
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
# Opt-in consumer test registration; inclusion alone downloads nothing and registers no tests.
include(WebFrontJasmine)

if(WEBFRONT_BUILD_AUTOMATION OR (WEBFRONT_ENABLE_TESTING AND
    (WEBFRONT_EMBED_CEF OR WEBFRONT_EMBED_WKWEBVIEW OR WEBFRONT_EMBED_WEBVIEW2 OR WEBFRONT_EMBED_WEBKITGTK)))
    webfront_default_embedded_frontend(_automation_default FALLBACK CEF)
    webfront_select_frontend(WEBFRONT_AUTOMATION_FRONTEND "${_automation_default}"
        "Developer automation frontend" EMBEDDED_ONLY REQUIRE_AVAILABLE)
    if(NOT ${WEBFRONT_FRONTEND_${WEBFRONT_AUTOMATION_FRONTEND}_OPTION})
        message(FATAL_ERROR "Automation needs an explicitly enabled embedded frontend")
    endif()
    add_executable(webfront-agent js/developer/tools/WebFrontAgent.cpp)
    target_link_libraries(webfront-agent PRIVATE WebFront WebFront_options WebFront_warnings)
    if(WIN32)
        target_link_libraries(webfront-agent PRIVATE ws2_32 bcrypt advapi32)
    endif()
    target_compile_definitions(webfront-agent PRIVATE WEBFRONT_AUTOMATION_FRONTEND_TYPE=${WEBFRONT_AUTOMATION_FRONTEND_TYPE})
    webfront_setup_frontends_for_target(webfront-agent)
endif()

if(WEBFRONT_ENABLE_TESTING OR WEBFRONT_ENABLE_FUZZING)
  include(TestHardening)
endif()

if(WEBFRONT_ENABLE_FUZZING)
  add_subdirectory(test/fuzz)
endif()

if(WEBFRONT_ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test/unit)
  add_subdirectory(test/web)
endif()

if(WEBFRONT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
