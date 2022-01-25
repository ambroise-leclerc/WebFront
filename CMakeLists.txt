cmake_minimum_required (VERSION 3.16)

project ("WebFront")

include(cmake/StandardProjectSettings.cmake)
include(cmake/PreventInSourceBuilds.cmake)
add_library(project_options INTERFACE)
target_compile_features(project_options INTERFACE cxx_std_20)
add_library(project_warnings INTERFACE)

include(cmake/Cache.cmake)

include(cmake/Linker.cmake)
configure_linker(project_options)

include(cmake/CompilerWarnings.cmake)
set_project_warnings(project_warnings)

include(cmake/Sanitizers.cmake)
enable_sanitizers(project_options)

include(cmake/Doxygen.cmake)
enable_doxygen()

include(cmake/StaticAnalyzers.cmake)
include(cmake/CPM.cmake)

option(BUILD_SHARED_LIBS "Enable compilation of shared libraries" OFF)
option(ENABLE_TESTING "Enable Test Builds" ON)
option(ENABLE_FUZZING "Enable Fuzzing Builds" OFF)

if(ENABLE_TESTING)
  enable_testing()
  add_subdirectory(test)
endif()

add_subdirectory(src)

#set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)

#find_package(Threads REQUIRED)
#if(WIN32)
#    find_library(WS2_32_LIBRARY ws2_32 REQUIRED)
#endif()

# Add source to this project's executable.
#add_executable (WebFront "WebFront.cpp" "WebFront.h")
#set_property(TARGET WebFront PROPERTY CMAKE_CXX_STANDARD 23)
#target_include_directories(WebFront PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdParty)

# TODO: Add tests and install targets if needed.
