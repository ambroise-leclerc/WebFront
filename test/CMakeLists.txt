CPMAddPackage("gh:catchorg/Catch2@3.15.2")
target_compile_features(Catch2 PRIVATE cxx_std_23)
target_compile_features(Catch2WithMain PRIVATE cxx_std_23)

find_package(Threads)
include(CTest)

set(TESTS_LIST)
list(APPEND TESTS_LIST HTTPServerTests.cpp ModuleHTTPServerTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
list(APPEND TESTS_LIST JSFunctionTests.cpp TypeErasedFunctionTests.cpp MessagesTests.cpp IndexFSTests.cpp)
list(APPEND TESTS_LIST JasmineFSTests.cpp FileSystemTests.cpp NativeFSTests.cpp ReactFSTests.cpp BabelFSTests.cpp)
list(APPEND TESTS_LIST DefaultFSTests.cpp StdnetShutdownTests.cpp TerminalTests.cpp TerminalFSTests.cpp)
list(APPEND TESTS_LIST WebFrontTests.cpp WebLinkTests.cpp CEFPathsTests.cpp)
if(WEBFRONT_USE_MDDLOG)
  list(APPEND TESTS_LIST MddlogLoggerTests.cpp MddlogTransportTests.cpp)
endif()
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE WebFront_warnings WebFront_options Catch2::Catch2WithMain WebFront)

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(tests)

if(ENABLE_COVERAGE)
  target_compile_options(tests PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(tests PUBLIC -fprofile-arcs -ftest-coverage)
endif()

if(WEBFRONT_USE_MDDLOG)
  # This consumer includes only the public header and links only the facade target.
  add_executable(mddlog_header_consumer MddlogHeaderConsumer.cpp)
  target_link_libraries(mddlog_header_consumer PRIVATE WebFront::mddlog)
  set_target_properties(mddlog_header_consumer PROPERTIES CXX_SCAN_FOR_MODULES OFF CXX_MODULE_STD OFF)
  add_test(NAME MddlogHeaderConsumer COMMAND mddlog_header_consumer)
endif()

# The documented newcomer commands and snippets must keep matching the built examples and CI.
add_test(NAME NewcomerDocsMatchExamples
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/CheckNewcomerDocs.cmake)

# A minimal parent project must be able to add_subdirectory(WebFront) and link WebFront::WebFront
# without building WebFront's examples or tests. Dependencies reuse this build's sources, so the
# test needs no network access.
include(FetchContent)
FetchContent_GetProperties(stdexec)
set(_consumer_dependency_args -DCPM_stdnet_SOURCE=${stdnet_SOURCE_DIR})
if(stdexec_POPULATED)
    list(APPEND _consumer_dependency_args -DFETCHCONTENT_SOURCE_DIR_STDEXEC=${stdexec_SOURCE_DIR})
endif()
add_test(NAME AddSubdirectoryConsumer
    COMMAND ${CMAKE_CTEST_COMMAND}
        --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/consumer ${CMAKE_CURRENT_BINARY_DIR}/consumer-build
        --build-generator ${CMAKE_GENERATOR}
        --build-config $<CONFIG>
        --build-options -DWEBFRONT_SOURCE_DIR=${PROJECT_SOURCE_DIR} ${_consumer_dependency_args}
            -DCMAKE_BUILD_TYPE=$<CONFIG>)
set_tests_properties(AddSubdirectoryConsumer PROPERTIES TIMEOUT 600)
