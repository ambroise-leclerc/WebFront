cmake_minimum_required (VERSION 3.18)

set(VERSION_STRING_REGEX "inline constexpr std::string_view version = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/WebFront.hpp" VERSION_STRING REGEX ${VERSION_STRING_REGEX})
string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

add_library(WebFront INTERFACE)

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

CPMAddPackage("gh:ambroise-leclerc/networking-ts-impl@1.12.0a")
CPMAddPackage("gh:ambroise-leclerc/CEF@118.7.1a")

# Ensure CEF test target is available for building
if(ENABLE_TESTING AND TARGET cef_sanity_test)
    # Add CEF test as a dependency to ensure it gets built
    set_target_properties(cef_sanity_test PROPERTIES
        EXCLUDE_FROM_ALL FALSE
    )
endif()

target_link_libraries(WebFront INTERFACE WebFront_options WebFront_warnings)
target_include_directories(WebFront INTERFACE ${CMAKE_SOURCE_DIR}/include)
include_directories(SYSTEM ${networking-ts-impl_SOURCE_DIR}/include)

option(BUILD_SHARED_LIBS "Enable compilation of shared libraries" OFF)
option(ENABLE_TESTING "Enable Test Builds" ON)
option(ENABLE_FUZZING "Enable Fuzzing Builds" OFF)

add_compile_definitions(_WIN32_WINNT=0x0501)

if(ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test)
  add_subdirectory(webtest)
  
  # Ensure CEF test is built when running tests
  if(TARGET cef_sanity_test)
    add_dependencies(tests cef_sanity_test)
  endif()
endif()

add_subdirectory(src)
