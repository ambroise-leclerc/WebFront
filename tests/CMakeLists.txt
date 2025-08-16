cmake_minimum_required(VERSION 3.17)
project(WebFront_tests)


set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(..)
include_directories(3rdParty)

add_executable(helloworld helloworld.cpp)
set_property(TARGET helloworld PROPERTY CXX_STANDARD 20)
