cmake_minimum_required (VERSION 3.16)

set(VERSION_REGEX "inline constexpr std::string_view version =[ \t]+\"(.+)\".")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/WebFront.hpp" VERSION_STRING REGEX ${VERSION_REGEX})
string(REGEX REPLACE ${VERSION_REGEX} "\\1" VERSION_STRING "${VERSION_STRING}")
project(WebFront LANGUAGES CXX VERSION ${VERSION_STRING})

add_library(WebFront INTERFACE)
include(cmake/StandardProjectSettings.cmake)
include(cmake/PreventInSourceBuilds.cmake)
add_library(project_options INTERFACE)
target_compile_features(project_options INTERFACE cxx_std_23)
add_library(project_warnings INTERFACE)


include(cmake/Cache.cmake)

include(cmake/Linker.cmake)
configure_linker(project_options)

include(cmake/CompilerWarnings.cmake)
set_project_warnings(project_warnings)

include(cmake/Sanitizers.cmake)
enable_sanitizers(project_options)

configure_file(Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile)
include(cmake/Doxygen.cmake)
enable_doxygen()

include(cmake/StaticAnalyzers.cmake)
include(cmake/CPM.cmake)

CPMAddPackage("gh:ambroise-leclerc/networking-ts-impl@1.12.0")


target_link_libraries(WebFront INTERFACE project_options project_warnings )
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
