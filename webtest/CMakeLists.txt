# Include CEF support module
include(CEFSupport)

add_executable(webtest JasmineTest.cpp)

# Link basic libraries
target_link_libraries(webtest PRIVATE WebFront WebFront_warnings WebFront_options)

include(Frontends)
webfront_default_embedded_frontend(_webfront_test_frontend FALLBACK CEF)
webfront_select_frontend(WEBFRONT_WEBTEST_FRONTEND "${_webfront_test_frontend}" "Browser test frontend" EMBEDDED_ONLY REQUIRE_AVAILABLE)
target_compile_definitions(webtest PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})

if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "WebView2")
    add_test(NAME WebFrontWebView2Integration COMMAND webtest)
    set_tests_properties(WebFrontWebView2Integration PROPERTIES
        LABELS "web-integration" TIMEOUT 90
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/webtest RESOURCE_LOCK webfront_port_9002)
    add_executable(webview2_lifecycle WebView2Lifecycle.cpp WebView2HeaderConsumer.cpp)
    target_link_libraries(webview2_lifecycle PRIVATE WebFront WebFront_warnings WebFront_options)
    foreach(mode no-bridge early-close rejected-ready navigation-failure mta missing-runtime
                 closed-startup-timeout failed-startup-timeout late-navigation-failure path-encoding runtime-ready)
        add_test(NAME WebView2Lifecycle-${mode} COMMAND webview2_lifecycle ${mode})
        set_tests_properties(WebView2Lifecycle-${mode} PROPERTIES
            LABELS "web-integration" TIMEOUT 45 RESOURCE_LOCK webfront_port_9002
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/webtest)
    endforeach()
endif()

# Setup complete CEF integration for webtest (same as WebFrontApp)
# This includes:
# - CEF compilation configuration and linking
# - Platform-specific runtime file deployment  
# - Resource file copying (with Linux ICU fix)
# - Locale directory setup
setup_cef_for_target(webtest)

if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "CEF" AND WEBFRONT_EMBED_CEF AND UNIX AND NOT APPLE)
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
