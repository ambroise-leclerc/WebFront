find_package(Threads)

add_executable(WebFront WebFront.cpp)
target_link_libraries(WebFront PRIVATE project_options project_warnings Threads::Threads)
target_include_directories(WebFront PRIVATE ${CMAKE_SOURCE_DIR}/include SYSTEM ${CMAKE_SOURCE_DIR}/3rdParty)

