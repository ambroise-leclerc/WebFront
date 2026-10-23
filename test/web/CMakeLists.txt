add_executable(webtest JasmineTest.cpp)
target_link_libraries(webtest PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)

include(Frontends)
webfront_default_embedded_frontend(_webfront_test_frontend FALLBACK CEF)
webfront_select_frontend(WEBFRONT_WEBTEST_FRONTEND "${_webfront_test_frontend}" "Browser test frontend" EMBEDDED_ONLY REQUIRE_AVAILABLE)
target_compile_definitions(webtest PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})

webfront_setup_frontends_for_target(webtest)
# Keep the diagnostic CEF-off build; lifecycle cases require a validated test platform.
if(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_OPTION}
   AND CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_PLATFORMS)
    add_executable(frontend_lifecycle FrontendLifecycle.cpp)
    target_link_libraries(frontend_lifecycle PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(frontend_lifecycle PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})
    webfront_setup_frontends_for_target(frontend_lifecycle)
    add_dependencies(webtest frontend_lifecycle)
endif()
webfront_add_browser_tests(webtest "${WEBFRONT_WEBTEST_FRONTEND}" "${CMAKE_CURRENT_SOURCE_DIR}" frontend_lifecycle)

# The pinned bootstrap environment is downloaded and verified at configure time (or taken from WEBFRONT_BOOTSTRAP_DIR on a
# machine without network access); webfront-wf runs wf commands without interaction from it, or from an environment passed
# explicitly (the one built from the current sources).
include(WebFrontBootstrap)
if(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_OPTION}
   AND CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_PLATFORMS)
    webfront_provision_bootstrap(WEBFRONT_BOOTSTRAP_ARCHIVE URL "${WEBFRONT_BOOTSTRAP_URL}"
        INTEGRITY "${WEBFRONT_BOOTSTRAP_INTEGRITY}" DESTINATION "${CMAKE_BINARY_DIR}/bootstrap" LOCAL_DIR "${WEBFRONT_BOOTSTRAP_DIR}")
    add_executable(webfront-wf ../driver/WebFrontWf.cpp)
    target_link_libraries(webfront-wf PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(webfront-wf PRIVATE WEBFRONT_DRIVER_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE}
        WEBFRONT_BOOTSTRAP_ARCHIVE="${WEBFRONT_BOOTSTRAP_ARCHIVE}" WEBFRONT_BOOTSTRAP_INTEGRITY="${WEBFRONT_BOOTSTRAP_INTEGRITY}")
    webfront_setup_frontends_for_target(webfront-wf)
endif()

# Exercise actual CEF command IDs, context-menu filtering and DevTools creation in the vendored runtime.
if(WEBFRONT_EMBED_CEF AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(cef_content_policy CEFContentPolicy.cpp)
    target_link_libraries(cef_content_policy PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    webfront_setup_frontends_for_target(cef_content_policy)
    add_dependencies(webtest cef_content_policy)
    webfront_find_xvfb(_policy_xvfb)
    if(_policy_xvfb)
        add_test(NAME CEFContentPolicy COMMAND "${_policy_xvfb}" -a $<TARGET_FILE:cef_content_policy>)
        webfront_set_browser_test_properties(CEFContentPolicy "${CMAKE_CURRENT_SOURCE_DIR}" 45)
    endif()
endif()

# Optional offline fixtures are provisioned by maintainers, never downloaded by CTest.
set(WEBFRONT_DEVELOPER_FIXTURES "" CACHE PATH "Prepared browser developer environment/registry fixtures")
if(WEBFRONT_DEVELOPER_FIXTURES
   AND ${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_OPTION}
   AND CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_PLATFORMS)
    if(NOT EXISTS "${WEBFRONT_DEVELOPER_FIXTURES}/provision.json")
        message(FATAL_ERROR "Prepare WEBFRONT_DEVELOPER_FIXTURES with js/developer/tools/build.mjs and fixtures.mjs")
    endif()
    add_executable(developer_integration DeveloperTest.cpp)
    target_link_libraries(developer_integration PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(developer_integration PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})
    webfront_setup_frontends_for_target(developer_integration)
    set(_developer_command "$<TARGET_FILE:developer_integration>")
    if(WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_XVFB_LINUX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        webfront_find_xvfb(_developer_xvfb)
        if(NOT _developer_xvfb)
            message(FATAL_ERROR "Developer browser integration requires xvfb-run on this frontend")
        endif()
        set(_developer_command "${_developer_xvfb}" -a ${_developer_command})
    endif()
    add_test(NAME DeveloperBrowserIntegration COMMAND ${_developer_command}
        "${WEBFRONT_DEVELOPER_FIXTURES}" "${CMAKE_CURRENT_BINARY_DIR}/developer-generated")
    webfront_set_browser_test_properties(DeveloperBrowserIntegration "${CMAKE_CURRENT_SOURCE_DIR}" 180)
    set_tests_properties(DeveloperBrowserIntegration PROPERTIES FIXTURES_SETUP DeveloperGenerated)
    # The driver on the environment built from the current sources: statuses, output and reports reach CTest.
    if(TARGET webfront-wf)
        file(READ "${WEBFRONT_DEVELOPER_FIXTURES}/provision.json" _provision)
        string(JSON _environment_integrity GET "${_provision}" integrity)
        set(_wf_project "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-project")
        set(_wf_run "${CMAKE_COMMAND}" -DROOT=${PROJECT_SOURCE_DIR} -DDRIVER=$<TARGET_FILE:webfront-wf>
            -DPROJECT=${_wf_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work
            -DENVIRONMENT=${WEBFRONT_DEVELOPER_FIXTURES}/environment.tgz -DINTEGRITY=${_environment_integrity}
            -DREGISTRY=${WEBFRONT_DEVELOPER_FIXTURES}/registry)
        set(_developer_xvfb_prefix "")
        if(_developer_xvfb)
            set(_developer_xvfb_prefix "${_developer_xvfb}" -a)
            set(_wf_run ${_developer_xvfb_prefix} ${_wf_run})
        endif()
        add_test(NAME WfDriverProject
            COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR}/test/fixtures/wf-project -DPROJECT=${_wf_project}
                -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareWfProject.cmake)
        set_tests_properties(WfDriverProject PROPERTIES FIXTURES_SETUP WfDriverProject LABELS "web-integration" TIMEOUT 30)

        # webfront_add_driver_test(<name> <expected status> <wf command words separated by |>
        #     [OUTPUT <regex>] [REPORT <regex>] [DRIVER_TIMEOUT <seconds>] [FORGE <protocol>:<api>]
        #     [ENVIRONMENT <archive> INTEGRITY <sri>] [REQUIRES <fixture>] [PROVIDES <fixture>])
        function(webfront_add_driver_test name status command)
            cmake_parse_arguments(ARG "" "OUTPUT;REPORT;DRIVER_TIMEOUT;FORGE;ENVIRONMENT;INTEGRITY;REQUIRES;PROVIDES" "" ${ARGN})
            set(options -DEXPECT_STATUS=${status} "-DCOMMAND=${command}")
            foreach(option OUTPUT REPORT)
                if(ARG_${option})
                    list(APPEND options "-DEXPECT_${option}=${ARG_${option}}")
                endif()
            endforeach()
            foreach(option FORGE ENVIRONMENT INTEGRITY)
                if(ARG_${option})
                    list(APPEND options "-D${option}=${ARG_${option}}")
                endif()
            endforeach()
            if(ARG_DRIVER_TIMEOUT)
                list(APPEND options -DTIMEOUT=${ARG_DRIVER_TIMEOUT})
            endif()
            add_test(NAME ${name} COMMAND ${_wf_run} ${options} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(${name} "${CMAKE_CURRENT_SOURCE_DIR}" 120)
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "WfDriverProject;${ARG_REQUIRES}")
            if(ARG_PROVIDES)
                set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${ARG_PROVIDES})
            endif()
        endfunction()

        # The fixture manifest has no jasmine-core: wf test asks for it, and wf install resolves it from the local registry.
        webfront_add_driver_test(WfDriverInstall 0 "install|jasmine-core" OUTPUT "Installed 1 packages" PROVIDES WfDriverInstalled)
        webfront_add_driver_test(WfDriverTestPasses 0 "test|adds" OUTPUT "PASSED: 1 spec" REPORT "\"status\":\"passed\"" REQUIRES WfDriverInstalled)
        # A failing command: its status, its output and the structured report reach CTest.
        webfront_add_driver_test(WfDriverTestFails 1 "test|fails" OUTPUT "FAILED: 1 spec, 1 failure" REPORT "\"failed\":1" REQUIRES WfDriverInstalled)
        webfront_add_driver_test(WfDriverUnknownCommand 1 "nosuchcommand" OUTPUT "nosuchcommand")
        webfront_add_driver_test(WfDriverTimeout 71 "run|sleep" OUTPUT "time limit of the driver" DRIVER_TIMEOUT 4)
        webfront_add_driver_test(WfDriverWrongIntegrity 70 "list" OUTPUT "Integrity check failed"
            INTEGRITY "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==")
        webfront_add_driver_test(WfDriverMissingArchive 70 "list" OUTPUT "archive not found"
            ENVIRONMENT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work/absent.tgz")
        webfront_add_driver_test(WfDriverIncompatibleApi 70 "list" OUTPUT "api version 2, this application supports 1" FORGE "1:2")
        webfront_add_driver_test(WfDriverIncompatibleProtocol 70 "list" OUTPUT "protocol version 2, this application supports 1" FORGE "2:1")
        add_test(NAME WfDriverUsage COMMAND ${_developer_xvfb_prefix} $<TARGET_FILE:webfront-wf> --project "${_wf_project}")
        set_tests_properties(WfDriverUsage PROPERTIES PASS_REGULAR_EXPRESSION "A wf command is required" LABELS "web-integration" TIMEOUT 30)
    endif()
    # Build the actual browser-generated header only after the setup test has produced it.
    add_executable(developer_embedded EXCLUDE_FROM_ALL DeveloperEmbeddedTest.cpp)
    target_link_libraries(developer_embedded PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(developer_embedded PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE}
        WEBFRONT_PI_ASSETS_HEADER="${CMAKE_CURRENT_BINARY_DIR}/developer-generated/PiUI.hpp")
    webfront_setup_frontends_for_target(developer_embedded)
    add_test(NAME DeveloperEmbeddedBuild COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
        --config $<CONFIG> --target developer_embedded --parallel 2)
    # A nested build can regenerate CMake or relink shared outputs. Exclude every
    # running test in this tree, not only the other nested build (Windows locks executables).
    set_tests_properties(DeveloperEmbeddedBuild PROPERTIES FIXTURES_REQUIRED DeveloperGenerated
        FIXTURES_SETUP DeveloperEmbedded LABELS "web-integration" TIMEOUT 180
        RUN_SERIAL TRUE PROCESSORS 2)
    set(_embedded_command "$<TARGET_FILE:developer_embedded>")
    if(_developer_xvfb)
        set(_embedded_command "${_developer_xvfb}" -a ${_embedded_command})
    endif()
    add_test(NAME DeveloperEmbeddedIntegration COMMAND ${_embedded_command})
    webfront_set_browser_test_properties(DeveloperEmbeddedIntegration "${CMAKE_CURRENT_SOURCE_DIR}" 60)
    set_tests_properties(DeveloperEmbeddedIntegration PROPERTIES FIXTURES_REQUIRED DeveloperEmbedded)
    add_executable(life_embedded EXCLUDE_FROM_ALL LifeEmbeddedTest.cpp)
    target_link_libraries(life_embedded PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(life_embedded PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE}
        WEBFRONT_LIFE_ASSETS_HEADER="${CMAKE_CURRENT_BINARY_DIR}/developer-generated/LifeUI.hpp")
    webfront_setup_frontends_for_target(life_embedded)
    add_test(NAME LifeEmbeddedBuild COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
        --config $<CONFIG> --target life_embedded --parallel 2)
    # Keep the same whole-tree exclusion as DeveloperEmbeddedBuild.
    set_tests_properties(LifeEmbeddedBuild PROPERTIES FIXTURES_REQUIRED DeveloperGenerated
        FIXTURES_SETUP LifeEmbedded LABELS "web-integration" TIMEOUT 180
        RUN_SERIAL TRUE PROCESSORS 2)
    set(_life_command "$<TARGET_FILE:life_embedded>")
    if(_developer_xvfb)
        set(_life_command "${_developer_xvfb}" -a ${_life_command})
    endif()
    add_test(NAME LifeEmbeddedIntegration COMMAND ${_life_command})
    webfront_set_browser_test_properties(LifeEmbeddedIntegration "${CMAKE_CURRENT_SOURCE_DIR}" 60)
    set_tests_properties(LifeEmbeddedIntegration PROPERTIES FIXTURES_REQUIRED LifeEmbedded)
    if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "WKWebView")
        get_filename_component(_developer_wk_plist "${PROJECT_SOURCE_DIR}/test/cmake/WKWebViewTestInfo.plist.in" ABSOLUTE)
        foreach(target developer_integration developer_embedded life_embedded)
            string(REPLACE "_" "-" identifier "${target}")
            set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_GUI_IDENTIFIER "org.webfront.tests.${identifier}"
                MACOSX_BUNDLE_INFO_PLIST "${_developer_wk_plist}")
        endforeach()
    endif()
endif()
