cmake_minimum_required(VERSION 3.31)
project(WebFrontPtyExample LANGUAGES CXX)

add_subdirectory(../.. webfront)
add_executable(WebFrontPtyExample main.cpp)
target_link_libraries(WebFrontPtyExample PRIVATE WebFront::WebFront)

# Stage a complete wf project in the build tree, with the maintained adapter.
foreach(file IN ITEMS main.mjs webfront.project.json webfront-packages.lock.json)
    configure_file("${file}" "page/${file}" COPYONLY)
endforeach()
configure_file("../../js/pty/index.mjs" "page/terminal.mjs" COPYONLY)
