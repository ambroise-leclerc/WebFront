Include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v2.13.8
)
FetchContent_MakeAvailable(Catch2)

find_package(Threads)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/contrib)
include(CTest)
include(Catch)

add_library(catch_main STATIC catch_main.cpp)
target_link_libraries(catch_main PUBLIC Catch2::Catch2)
target_link_libraries(catch_main PRIVATE project_options Threads::Threads)


add_executable(tests "HTTPServerTests.cpp" "EncodingsTests.cpp" "WebSocketTests.cpp")
target_link_libraries(tests PRIVATE project_warnings project_options catch_main)
target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/include SYSTEM ${CMAKE_SOURCE_DIR}/3rdParty)

catch_discover_tests(tests TEST_PREFIX "unittests." REPORTER xml OUTPUT_DIR . OUTPUT_PREFIX "unittests." OUTPUT_SUFFIX .xml)

