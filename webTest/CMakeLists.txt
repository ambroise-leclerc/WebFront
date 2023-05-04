find_package(Threads)

add_executable(webtest JasmineTest.cpp)
target_link_libraries(webtest PRIVATE project_options project_warnings Threads::Threads)
target_include_directories(webtest PRIVATE ${CMAKE_SOURCE_DIR}/include SYSTEM ${networking-ts-impl_SOURCE_DIR}/include)

