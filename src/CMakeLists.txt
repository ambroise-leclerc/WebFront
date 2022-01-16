
add_executable(WebFront Webfront.cpp)
target_link_libraries(WebFront PRIVATE project_options project_warnings)
target_include_directories(WebFront PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/3rdParty)