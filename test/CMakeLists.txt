option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)

CPMAddPackage("gh:doctest/doctest@2.4.9")

find_package(Threads)
include(CTest)

set(TESTS_LIST doctest_main.cpp)
list(APPEND TESTS_LIST HTTPServerTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
list(APPEND TESTS_LIST JSFunctionTests.cpp TypeErasedFunctionTests.cpp MessagesTests.cpp IndexFSTests.cpp)
list(APPEND TESTS_LIST JasmineFSTests.cpp FileSystemTests.cpp NativeFSTests.cpp)
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE project_warnings project_options doctest::doctest WebFront)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(tests)

if(ENABLE_TEST_COVERAGE)
  target_compile_options(tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()
