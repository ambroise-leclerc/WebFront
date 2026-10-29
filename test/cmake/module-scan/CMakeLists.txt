# Configure-only project exercising cmake/ModuleScan.cmake with a stand-in for WebFront::WebFront.
cmake_minimum_required(VERSION 3.31)
project(ModuleScanProbe LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${WEBFRONT_SOURCE_DIR}/cmake")
include(ModuleScan)

add_library(WebFront INTERFACE)
add_library(WebFront::WebFront ALIAS WebFront)

if(SCENARIO STREQUAL "variable")
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()
add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE WebFront::WebFront)
if(SCENARIO STREQUAL "function")
    webfront_disable_module_scan(probe)
endif()
if(SCENARIO STREQUAL "unrelated")
    set_target_properties(probe PROPERTIES CXX_SCAN_FOR_MODULES ON)
    set_property(TARGET probe PROPERTY LINK_LIBRARIES "")
endif()

set(WEBFRONT_MODULE_SCAN_CHECK_FORCE ON)
webfront_check_module_scan()
