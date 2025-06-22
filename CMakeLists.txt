cmake_minimum_required (VERSION 3.18)

set(VERSION_STRING_REGEX "inline constexpr std::string_view version = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/WebFront.hpp" VERSION_STRING REGEX ${VERSION_STRING_REGEX})
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

add_library(WebFront INTERFACE)

# Static linkage for CEF compatibility
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(StandardProjectSettings)
include(PreventInSourceBuilds)
add_library(WebFront_options INTERFACE)
target_compile_features(WebFront_options INTERFACE cxx_std_23)
if(APPLE)
  target_compile_definitions(WebFront_options INTERFACE __cpp_lib_format=202207L)
endif()

add_library(WebFront_warnings INTERFACE)

include(Cache)

include(Linker)
configure_linker(WebFront_options)

include(CompilerWarnings)
set_project_warnings(WebFront_warnings)

include(Sanitizers)
enable_sanitizers(WebFront_options)

configure_file(Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile)
include(Doxygen)
enable_doxygen()

include(StaticAnalyzers)
include(CPM)

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

CPMAddPackage("gh:ambroise-leclerc/networking-ts-impl@1.12.0a")

# Skip CEF tests on macOS to avoid missing macro issues
if(APPLE)
    set(CEF_SKIP_TESTS ON CACHE BOOL "Skip CEF tests on macOS" FORCE)
    set(CEF_BUILD_TESTS OFF CACHE BOOL "Disable CEF test building" FORCE)
    set(CEF_WRAPPER_BUILD_SKIP ON CACHE BOOL "Skip CEF wrapper build on macOS" FORCE)
endif()

CPMAddPackage("gh:ambroise-leclerc/CEF@137.0.17")

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
            "cmake_minimum_required(VERSION 3.21)\n"
            "project(libcef_dll_wrapper)\n"
            "\n"
            "# Create an empty interface target for compatibility\n"
            "add_library(libcef_dll_wrapper INTERFACE)\n"
            "target_include_directories(libcef_dll_wrapper INTERFACE \${CMAKE_CURRENT_SOURCE_DIR})\n"
        )
        message(STATUS "Created minimal libcef_dll_wrapper for macOS")
    endif()
endif()

target_link_libraries(WebFront INTERFACE WebFront_options WebFront_warnings)
target_include_directories(WebFront INTERFACE ${CMAKE_SOURCE_DIR}/include)
# Ajouter les headers CEF à WebFront comme headers système pour éviter les warnings
if(EXISTS "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}/include")
endif()
include_directories(SYSTEM ${networking-ts-impl_SOURCE_DIR}/include)

option(BUILD_SHARED_LIBS "Enable compilation of shared libraries" OFF)
option(ENABLE_TESTING "Enable Test Builds" ON)
option(ENABLE_FUZZING "Enable Fuzzing Builds" OFF)

add_compile_definitions(_WIN32_WINNT=0x0501)

if(ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test)
  add_subdirectory(webtest)
endif()

add_subdirectory(src)
