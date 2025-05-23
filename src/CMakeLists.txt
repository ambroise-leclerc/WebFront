find_package(Threads)

add_executable(WebFrontApp HelloWorld.cpp)
target_link_libraries(WebFrontApp PRIVATE WebFront Threads::Threads)

if(WIN32 AND WebView2_FOUND)
  target_include_directories(WebFrontApp PRIVATE ${WebView2_INCLUDE_DIRS})
  target_link_libraries(WebFrontApp PRIVATE ${WebView2_LIBRARY})
  message(STATUS "Configuring HelloWorld with WebView2 SDK.")
  message(STATUS "WebView2 Include Dirs: ${WebView2_INCLUDE_DIRS}")
  message(STATUS "WebView2 Library: ${WebView2_LIBRARY}")
else()
  if(WIN32)
    message(WARNING "WebView2 SDK not found. Windows build will not include WebView2 features.")
  endif()
endif()

add_custom_command(
    OUTPUT WebFront.js
    COMMAND tsc
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})