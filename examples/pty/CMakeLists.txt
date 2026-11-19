cmake_minimum_required(VERSION 3.31)
project(WebFrontPtyExample LANGUAGES CXX)

add_subdirectory(../.. webfront)
add_executable(WebFrontPtyExample main.cpp)
target_link_libraries(WebFrontPtyExample PRIVATE WebFront::WebFront)
