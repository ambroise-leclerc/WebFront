# Loaded by webfront_include_frontend_support() only when WEBFRONT_EMBED_CEF is enabled.
# CPM scopes the BUILD_TESTING override to CEF, preserving the caller's BUILD_TESTING bindings.
include("${CMAKE_CURRENT_LIST_DIR}/CEFSupport.cmake")

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

# Inherited SDK flags; CEF 154 does not honor all of them. CPM below controls CTest registration.
set(CEF_SKIP_TESTS ON CACHE BOOL "Skip CEF tests" FORCE)
set(CEF_BUILD_TESTS OFF CACHE BOOL "Disable CEF test building" FORCE)
set(BUILD_CEF_TESTS OFF CACHE BOOL "Disable CEF test building" FORCE)
set(CEF_BUILD_LIBCEF_DLL_WRAPPER ON CACHE BOOL "Enable CEF wrapper" FORCE)

# Preserve inherited dependency build options
set(OPTION_BUILD_TESTS OFF CACHE BOOL "Disable all test building" FORCE)

# Additional inherited CEF test options
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

# CEF 154 creates its demo targets unconditionally. Keep them out of the default build;
# BUILD_TESTING=OFF above prevents CEF's own CTest registration on the first configure.
foreach(_webfront_cef_test_target cef_sanity_test cef_resources_test cef_window_test)
    if(TARGET ${_webfront_cef_test_target})
        set_property(TARGET ${_webfront_cef_test_target} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()
unset(_webfront_cef_test_target)

# Never substitute an empty interface library for the wrapper, or edit cached SDK sources.
if(NOT TARGET libcef_dll_wrapper)
    message(FATAL_ERROR "CEF requires the real libcef_dll_wrapper static library; the SDK wrapper is missing")
endif()
get_target_property(_webfront_cef_wrapper_type libcef_dll_wrapper TYPE)
if(NOT _webfront_cef_wrapper_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "CEF requires the real libcef_dll_wrapper static library; got ${_webfront_cef_wrapper_type}")
endif()
unset(_webfront_cef_wrapper_type)

# Ajouter les headers CEF à WebFront comme headers système pour éviter les warnings
if(EXISTS "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}")
  target_include_directories(WebFront SYSTEM INTERFACE "${CEF_ROOT}/include")
endif()
