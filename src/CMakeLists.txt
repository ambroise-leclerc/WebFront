find_package(Threads)

add_executable(WebFrontApp HelloWorld.cpp)
target_link_libraries(WebFrontApp PRIVATE WebFront Threads::Threads)

add_custom_command(
    OUTPUT WebFront.js
    COMMAND tsc
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})