# Smoke test: a minimal parent project embedding WebFront with add_subdirectory and linking
# WebFront::WebFront. It must not build WebFront's examples or tests.
cmake_minimum_required(VERSION 3.31)
project(WebFrontConsumer LANGUAGES CXX)

set(WEBFRONT_SOURCE_DIR "" CACHE PATH "Path of the WebFront checkout")
add_subdirectory("${WEBFRONT_SOURCE_DIR}" webfront)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE WebFront::WebFront)

if(TARGET HelloWorld OR TARGET tests OR TARGET webtest)
    message(FATAL_ERROR "WebFront built examples or tests although it is not the top-level project")
endif()
