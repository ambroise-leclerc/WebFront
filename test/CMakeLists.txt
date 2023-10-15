option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)

CPMAddPackage("gh:catchorg/Catch2@3.4.0")
target_compile_features(Catch2 PRIVATE cxx_std_23)
target_compile_features(Catch2WithMain PRIVATE cxx_std_23)

find_package(Threads)
include(CTest)

set(TESTS_LIST)
list(APPEND TESTS_LIST HTTPServerTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
list(APPEND TESTS_LIST JSFunctionTests.cpp TypeErasedFunctionTests.cpp MessagesTests.cpp IndexFSTests.cpp)
list(APPEND TESTS_LIST JasmineFSTests.cpp FileSystemTests.cpp NativeFSTests.cpp ReactFSTests.cpp BabelFSTests.cpp)
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE project_warnings project_options Catch2::Catch2WithMain WebFront)

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(tests)

if(ENABLE_TEST_COVERAGE)
  target_compile_options(tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()
