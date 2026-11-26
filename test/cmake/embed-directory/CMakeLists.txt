# A parent project that embeds a plain interface directory with webfront_embed_directory() (#675) and serves it: the
# consumer compares every file the embedded provider serves with the same file served from the disk.
cmake_minimum_required(VERSION 3.31)
project(WebFrontEmbedDirectoryConsumer LANGUAGES CXX)

set(WEBFRONT_SOURCE_DIR "" CACHE PATH "Path of the WebFront checkout")
set(UI_DIR "" CACHE PATH "The directory to embed")
if(CPM_MODULE)
    # A nested consumer configure reuses the already downloaded CPM module offline.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cmake")
    get_filename_component(cpm_filename "${CPM_MODULE}" NAME)
    file(COPY_FILE "${CPM_MODULE}" "${CMAKE_BINARY_DIR}/cmake/${cpm_filename}" ONLY_IF_DIFFERENT)
endif()
add_subdirectory("${WEBFRONT_SOURCE_DIR}" webfront)

add_executable(embed_consumer main.cpp)
target_link_libraries(embed_consumer PRIVATE WebFront::WebFront)
# Hidden files are skipped, as an application skips .DS_Store; the driver checks that they are not served.
webfront_embed_directory(embed_consumer "${UI_DIR}" CLASS ConsumerUI NAMESPACE consumer::ui EXCLUDE "(^|/)[.]")
# The default class and namespace, through an alias, from the same directory.
add_executable(embed_default main.cpp)
add_executable(consumer::default ALIAS embed_default)
target_link_libraries(embed_default PRIVATE WebFront::WebFront)
webfront_embed_directory(consumer::default "${UI_DIR}" EXCLUDE "(^|/)[.]")
target_compile_definitions(embed_default PRIVATE EMBED_DEFAULT_NAMES)
# The application of docs/EMBEDDED_ASSETS.md, compiled only.
add_executable(app app.cpp)
target_link_libraries(app PRIVATE WebFront::WebFront)
webfront_embed_directory(app "${UI_DIR}" CLASS AppUI NAMESPACE app EXCLUDE "(^|/)[.]")
