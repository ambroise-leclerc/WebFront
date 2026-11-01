# Smoke test: a minimal parent project embedding WebFront with add_subdirectory and linking
# WebFront::WebFront. It must not build WebFront's examples or tests.
cmake_minimum_required(VERSION 3.31)
# Exercise language-dependent usage requirements even though the executable is C++.
project(WebFrontConsumer LANGUAGES C CXX)

set(WEBFRONT_SOURCE_DIR "" CACHE PATH "Path of the WebFront checkout")
if(CPM_MODULE)
    # A nested consumer configure reuses the already downloaded CPM module offline.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cmake")
    get_filename_component(cpm_filename "${CPM_MODULE}" NAME)
    file(COPY_FILE "${CPM_MODULE}" "${CMAKE_BINARY_DIR}/cmake/${cpm_filename}" ONLY_IF_DIFFERENT)
endif()
add_subdirectory("${WEBFRONT_SOURCE_DIR}" webfront)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE WebFront::WebFront)
webfront_use_native_window(consumer)
if(EXPECT_NATIVE)
    get_target_property(definitions consumer COMPILE_DEFINITIONS)
    if(NOT "WEBFRONT_NATIVE_WINDOW" IN_LIST definitions)
        message(FATAL_ERROR "The platform's native frontend was not selected")
    endif()
    target_compile_definitions(consumer PRIVATE WEBFRONT_CONSUMER_EXPECT_NATIVE)
endif()
target_compile_definitions(consumer PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:WEBFRONT_CONSUMER_CXX=1>")

# Registration from the parent scope must not rely on WebFront's private module path/registry.
# This test configures/builds the consumer only; the dummy JS is never executed as Jasmine.
enable_testing()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dummy.js" "// configuration fixture\n")
webfront_add_jasmine_test(NAME PublicJasmineRegistration TARGET consumer
    JASMINE "${CMAKE_CURRENT_BINARY_DIR}/dummy.js" SPECS "${CMAKE_CURRENT_BINARY_DIR}/dummy.js")
# The native grant (PROGRAMS) is accepted from a parent project too, with a generator expression for the program.
webfront_add_jasmine_test(NAME PublicJasmineHostRegistration TARGET consumer
    JASMINE "${CMAKE_CURRENT_BINARY_DIR}/dummy.js" SPECS "${CMAKE_CURRENT_BINARY_DIR}/dummy.js"
    PROGRAMS "$<TARGET_FILE:consumer>")
get_property(engines GLOBAL PROPERTY WEBFRONT_JASMINE_FRONTENDS)
if(NOT "WKWebView" IN_LIST engines)
    message(FATAL_ERROR "Public Jasmine frontend metadata was lost across add_subdirectory")
endif()

# Sanitizer, coverage and linker options are internal: consumers must not inherit them.
get_target_property(_webfront_libs WebFront::WebFront INTERFACE_LINK_LIBRARIES)
if("WebFront_options" IN_LIST _webfront_libs OR "WebFront_warnings" IN_LIST _webfront_libs OR "WebFront_test_hardening" IN_LIST _webfront_libs)
    message(FATAL_ERROR "WebFront::WebFront propagates internal options/warnings to consumers: ${_webfront_libs}")
endif()

if(TARGET HelloWorld OR TARGET tests OR TARGET webtest)
    message(FATAL_ERROR "WebFront built examples or tests although it is not the top-level project")
endif()

# Evaluate transitive usage requirements as the generator sees them; UBSan and
# automatic-variable initialization have no portable preprocessor detection.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/consumer-options-$<CONFIG>-$<COMPILE_LANGUAGE>.cmake" CONTENT
"set(consumerCompileOptions [==[$<TARGET_PROPERTY:consumer,COMPILE_OPTIONS>]==])
set(consumerCompileDefinitions [==[$<TARGET_PROPERTY:consumer,COMPILE_DEFINITIONS>]==])
set(consumerLinkOptions [==[$<TARGET_PROPERTY:consumer,LINK_OPTIONS>]==])
" TARGET consumer)
add_custom_target(check_consumer_options ALL
    COMMAND ${CMAKE_COMMAND} "-DOPTIONS_FILE=${CMAKE_CURRENT_BINARY_DIR}/consumer-options-$<CONFIG>-CXX.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckOptions.cmake"
    VERBATIM)
