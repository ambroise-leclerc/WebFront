CPMAddPackage("gh:catchorg/Catch2@3.15.2")
target_compile_features(Catch2 PRIVATE cxx_std_23)
target_compile_features(Catch2WithMain PRIVATE cxx_std_23)

find_package(Threads)
include(CTest)

set(TESTS_LIST)
list(APPEND TESTS_LIST HTTPServerTests.cpp ModuleHTTPServerTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
list(APPEND TESTS_LIST JSFunctionTests.cpp TypeErasedFunctionTests.cpp MessagesTests.cpp IndexFSTests.cpp)
list(APPEND TESTS_LIST JasmineFSTests.cpp FileSystemTests.cpp NativeFSTests.cpp ReactFSTests.cpp BabelFSTests.cpp)
list(APPEND TESTS_LIST DefaultFSTests.cpp)
list(APPEND TESTS_LIST WebFrontTests.cpp WebLinkTests.cpp)
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE WebFront_warnings WebFront_options Catch2::Catch2WithMain WebFront)

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(tests)

if(ENABLE_COVERAGE)
  target_compile_options(tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()
