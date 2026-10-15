# Include CEF support module
include(CEFSupport)

add_executable(webtest JasmineTest.cpp)

# Link basic libraries
target_link_libraries(webtest PRIVATE WebFront WebFront_warnings WebFront_options)

# Local npm registry for the wf tool manager specs (webtest/js/wf-specs.mjs), and the project
# directory the installed tools go to, emptied when the test starts.
set(WEBTEST_REGISTRY_DIR ${CMAKE_CURRENT_BINARY_DIR}/registry)
add_custom_command(
    OUTPUT ${WEBTEST_REGISTRY_DIR}/webfront-hello.json
    COMMAND ${CMAKE_COMMAND} -DOUTPUT_DIR=${WEBTEST_REGISTRY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/MakeToolRegistry.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/MakeToolRegistry.cmake
    COMMENT "Building the local npm registry of the browser test"
    VERBATIM
)
add_custom_target(webtest-registry DEPENDS ${WEBTEST_REGISTRY_DIR}/webfront-hello.json)
add_dependencies(webtest webtest-registry)
target_compile_definitions(webtest PRIVATE
    WEBTEST_REGISTRY_DIR="${WEBTEST_REGISTRY_DIR}"
    WEBTEST_TOOL_PROJECT_DIR="${CMAKE_CURRENT_BINARY_DIR}/tool-project")

# Setup complete CEF integration for webtest (same as WebFrontApp)
# This includes:
# - CEF compilation configuration and linking
# - Platform-specific runtime file deployment  
# - Resource file copying (with Linux ICU fix)
# - Locale directory setup
setup_cef_for_target(webtest)

if(WEBFRONT_EMBED_CEF AND UNIX AND NOT APPLE)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    if(XVFB_RUN_EXECUTABLE)
        add_test(
            NAME WebFrontBrowserIntegration
            COMMAND ${XVFB_RUN_EXECUTABLE} -a $<TARGET_FILE:webtest>
        )
        set_tests_properties(WebFrontBrowserIntegration PROPERTIES
            LABELS "web-integration"
            TIMEOUT 60
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/webtest
            RESOURCE_LOCK webfront_port_9002
        )
    else()
        message(WARNING "xvfb-run was not found; the CEF browser integration test will not be registered")
    endif()
endif()
