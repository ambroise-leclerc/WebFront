# Include CEF support module
include(CEFSupport)

add_executable(webtest JasmineTest.cpp)

# Link basic libraries
target_link_libraries(webtest PRIVATE WebFront)

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
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webtest
            RESOURCE_LOCK webfront_port_9002
        )
    else()
        message(WARNING "xvfb-run was not found; the CEF browser integration test will not be registered")
    endif()
endif()
