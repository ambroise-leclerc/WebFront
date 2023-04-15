find_package(Threads)
CPMAddPackage("gh:ambroise-leclerc/networking-ts-impl@1.12.0")

add_executable(WebFront HelloWorld.cpp)
target_link_libraries(WebFront PRIVATE project_options project_warnings Threads::Threads)
target_include_directories(WebFront PRIVATE ${CMAKE_SOURCE_DIR}/include SYSTEM ${networking-ts-impl_SOURCE_DIR}/include)

add_custom_command(
    OUTPUT WebFront.js
    COMMAND tsc
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})