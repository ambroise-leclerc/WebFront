add_executable(webtest JasmineTest.cpp)
target_link_libraries(webtest PRIVATE project_options project_warnings)
target_include_directories(webtest PRIVATE ${CMAKE_SOURCE_DIR}/include)

