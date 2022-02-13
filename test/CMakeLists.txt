CPMAddPackage("gh:catchorg/Catch2@2.13.8")

find_package(Threads)
include(CTest)
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/contrib)
include(Catch)

add_library(catch_main STATIC catch_main.cpp)
target_link_libraries(catch_main PUBLIC Catch2::Catch2)
target_link_libraries(catch_main PRIVATE project_options Threads::Threads)

set(TESTS_LIST MessagesTests.cpp)
#list(APPEND TESTS_LIST HTTPServerTests.cpp EncodingsTests.cpp WebSocketTests.cpp LoggerTests.cpp MimeTypeTests.cpp)
#list(APPEND TESTS_LIST JSFunctionTests.cpp TypeErasedFunctionTests.cpp)
add_executable(tests ${TESTS_LIST})
target_link_libraries(tests PRIVATE project_warnings project_options catch_main)
target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/include SYSTEM ${CMAKE_SOURCE_DIR}/3rdParty)

catch_discover_tests(tests TEST_PREFIX "unittests." REPORTER xml OUTPUT_DIR . OUTPUT_PREFIX "unittests." OUTPUT_SUFFIX .xml)

