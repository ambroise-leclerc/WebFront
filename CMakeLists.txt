cmake_minimum_required (VERSION 3.18)

set(VERSION_REGEX "inline constexpr std::string_view version =[ \t]+\"(.+)\".")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/WebFront.hpp" VERSION_STRING REGEX ${VERSION_REGEX})
string(REGEX REPLACE ${VERSION_REGEX} "\\1" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

add_library(WebFront INTERFACE)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(StandardProjectSettings)
include(PreventInSourceBuilds)
add_library(WebFront_options INTERFACE)
target_compile_features(WebFront_options INTERFACE cxx_std_23)
target_compile_definitions(WebFront_options INTERFACE __cpp_lib_format=202207L)
set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API "2182bf5c-ef0d-489a-91da-49dbc3090d2a")
set(CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP ON)
if(MSVC)
  target_compile_options(WebFront_options INTERFACE /BuildStlModules)
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


target_link_libraries(WebFront INTERFACE WebFront_options WebFront_warnings)
target_include_directories(WebFront INTERFACE ${CMAKE_SOURCE_DIR}/include SYSTEM ${networking-ts-impl_SOURCE_DIR}/include)

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
