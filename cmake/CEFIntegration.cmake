# Loaded by webfront_include_frontend_support() only when WEBFRONT_EMBED_CEF is enabled.
# CPM scopes the BUILD_TESTING override to the CEF dependency, preserving caller settings.
include(CEFSupport)

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

# Additional aggressive test prevention for CEF
set(CEF_ENABLE_TESTS OFF CACHE BOOL "Disable CEF tests" FORCE)
set(ENABLE_CEF_TESTS OFF CACHE BOOL "Disable CEF tests" FORCE)
set(WITH_TESTS OFF CACHE BOOL "Disable all tests" FORCE)

# Dependency options create a normal variable in CPM's add_subdirectory scope. This overrides
# both normal and cache settings for CEF without modifying either binding in the caller.
# URI preserves the original shorthand's SYSTEM and EXCLUDE_FROM_ALL defaults.
# This scopes the normal binding, not arbitrary cache writes inside a dependency. Recheck CEF's
# BUILD_TESTING handling on SDK upgrades; CACHE FORCE could still modify the caller's cache.
CPMAddPackage(
    URI "gh:ambroise-leclerc/CEF@154.0.34"
    OPTIONS "BUILD_TESTING OFF"
)

# Workaround: CEF ships only release libs (iterator debug level 0). Prevent MSVC debug iterator level
# mismatch; the definitions propagate to every target linking WebFront (examples, tests, consumers).
if(MSVC)
    target_compile_definitions(WebFront INTERFACE _ITERATOR_DEBUG_LEVEL=0 _HAS_ITERATOR_DEBUGGING=0)
endif()

# Override CEF testing to prevent any test building on macOS and problematic platforms
if(APPLE)
    # Replace the CEF testing file to disable all tests
    file(WRITE "${CMAKE_BINARY_DIR}/_deps/cef-src/cmake/CEFTesting.cmake"
        "# CEF Testing disabled for macOS compatibility\n"
        "# Original file replaced to prevent test building issues\n"
        "message(STATUS \"CEF testing has been disabled for platform compatibility\")\n"
        "# Do not enable testing or add test subdirectory\n"
    )
    message(STATUS "Disabled CEF testing for macOS compatibility")
endif()

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

# Ajouter les headers CEF à WebFront comme headers système pour éviter les warnings
if(EXISTS "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}/include")
endif()
