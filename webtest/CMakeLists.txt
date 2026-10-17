add_executable(webtest JasmineTest.cpp)
target_link_libraries(webtest PRIVATE WebFront WebFront_warnings WebFront_options)

include(Frontends)
webfront_default_embedded_frontend(_webfront_test_frontend FALLBACK CEF)
webfront_select_frontend(WEBFRONT_WEBTEST_FRONTEND "${_webfront_test_frontend}" "Browser test frontend" EMBEDDED_ONLY REQUIRE_AVAILABLE)
target_compile_definitions(webtest PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})

webfront_setup_frontends_for_target(webtest)
webfront_add_browser_tests(webtest "${WEBFRONT_WEBTEST_FRONTEND}" "${CMAKE_CURRENT_SOURCE_DIR}")
