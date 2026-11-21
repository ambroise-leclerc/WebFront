add_executable(webtest JasmineTest.cpp)
target_compile_definitions(webtest PRIVATE WEBFRONT_TEST_MEDIA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/seek.webm")
target_link_libraries(webtest PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
# The host of the host-loop example, on the CPU renderer and, where a toolkit was found, on the CUDA one.
target_link_libraries(webtest PRIVATE WebFront_boing)
if(MSVC)
    # The shared browser suite instantiates enough bridge callbacks to exceed standard COFF's section limit.
    target_compile_options(webtest PRIVATE /bigobj)
endif()

include(Frontends)
webfront_default_embedded_frontend(_webfront_test_frontend FALLBACK CEF)
webfront_select_frontend(WEBFRONT_WEBTEST_FRONTEND "${_webfront_test_frontend}" "Browser test frontend" EMBEDDED_ONLY REQUIRE_AVAILABLE)
target_compile_definitions(webtest PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})

webfront_setup_frontends_for_target(webtest)

# webfront_add_test_program(<target> <source> [EXCLUDE_FROM_ALL] [DRIVER] [WITH_WEBTEST] [DEFINITIONS <definitions>...]):
# a hardened program on the browser test frontend, named WEBFRONT_TEST_FRONTEND (WEBFRONT_DRIVER_FRONTEND for DRIVER).
# WITH_WEBTEST builds it with webtest: the CEF job and the documented commands build only webtest and the driver.
function(webfront_add_test_program target source)
    cmake_parse_arguments(ARG "EXCLUDE_FROM_ALL;DRIVER;WITH_WEBTEST" "" "DEFINITIONS" ${ARGN})
    set(exclude)
    if(ARG_EXCLUDE_FROM_ALL)
        set(exclude EXCLUDE_FROM_ALL)
    endif()
    add_executable(${target} ${exclude} ${source})
    target_link_libraries(${target} PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    set(frontend WEBFRONT_TEST_FRONTEND)
    if(ARG_DRIVER)
        set(frontend WEBFRONT_DRIVER_FRONTEND)
    endif()
    target_compile_definitions(${target} PRIVATE ${frontend}=${WEBFRONT_WEBTEST_FRONTEND_TYPE} ${ARG_DEFINITIONS})
    webfront_setup_frontends_for_target(${target})
    if(ARG_WITH_WEBTEST)
        add_dependencies(webtest ${target})
    endif()
endfunction()

# webfront_add_web_step(<name> COMMAND <command>... [REQUIRES <fixtures>...] [PROVIDES <fixture>] [LOCK <resource>]
#     [TIMEOUT <seconds>] [PROPERTIES <property> <value>...]): a step of the browser tests (default timeout 30 s).
function(webfront_add_web_step name)
    cmake_parse_arguments(ARG "" "PROVIDES;LOCK;TIMEOUT" "COMMAND;REQUIRES;PROPERTIES" ${ARGN})
    add_test(NAME ${name} COMMAND ${ARG_COMMAND})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()
    set_tests_properties(${name} PROPERTIES LABELS "web-integration" TIMEOUT ${ARG_TIMEOUT} ${ARG_PROPERTIES})
    foreach(property REQUIRES:FIXTURES_REQUIRED PROVIDES:FIXTURES_SETUP LOCK:RESOURCE_LOCK)
        string(REPLACE ":" ";" property "${property}")
        list(GET property 0 argument)
        list(GET property 1 property)
        if(ARG_${argument})
            set_tests_properties(${name} PROPERTIES ${property} "${ARG_${argument}}")
        endif()
    endforeach()
endfunction()

# Keep the diagnostic CEF-off build; lifecycle cases require a validated test platform.
if(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_OPTION}
   AND CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_PLATFORMS)
    webfront_add_test_program(frontend_lifecycle FrontendLifecycle.cpp WITH_WEBTEST)
    add_test(NAME FrontendLifecycleWatchdog COMMAND "${CMAKE_COMMAND}"
        "-DPROBE=$<TARGET_FILE:frontend_lifecycle>" -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckLifecycleWatchdog.cmake")
    set_tests_properties(FrontendLifecycleWatchdog PROPERTIES TIMEOUT 10 LABELS fast)
endif()

# Several applications with no configured port: all start, answer on the port they announce and stop (#617). The first one
# takes 9002 when it is free, hence the lock.
add_executable(multi_instance MultiInstance.cpp)
target_link_libraries(multi_instance PRIVATE WebFront WebFront_warnings WebFront_options)
webfront_setup_frontends_for_target(multi_instance)
foreach(_count 2 48)
    add_test(NAME MultiInstance-${_count} COMMAND multi_instance ${_count})
    set_tests_properties(MultiInstance-${_count} PROPERTIES LABELS "web-integration" TIMEOUT 60 RESOURCE_LOCK webfront_port_9002)
endforeach()

# Keep the stage values in the HTML probes synchronized with the C++ enum, including in CEF-off builds.
add_test(NAME FrontendLifecycleStageValues COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckLifecycleStageValues.cmake")
set_tests_properties(FrontendLifecycleStageValues PROPERTIES TIMEOUT 10 LABELS fast)
# The reproducibility proof compares every file it lists, and refuses a list split by its caller.
add_test(NAME CompareFilesWholeList COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/compare-files-check" -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckCompareFiles.cmake")
set_tests_properties(CompareFilesWholeList PROPERTIES TIMEOUT 10 LABELS fast)

# Manual validation of the interface-thread executor and the native window (#450) with a real browser, window and native
# dialog: never built or run by CTest/CI. See docs/validation/native-integration-450.md.
add_executable(native_integration_manual EXCLUDE_FROM_ALL NativeIntegrationManual.cpp)
target_link_libraries(native_integration_manual PRIVATE WebFront WebFront_warnings WebFront_options)
if(TARGET frontend_lifecycle)
    target_compile_definitions(native_integration_manual PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})
    webfront_setup_frontends_for_target(native_integration_manual)
endif()

# The runner passes arguments literally and kills the whole process tree at its deadline (POSIX shell wrapper).
if(UNIX)
    add_test(NAME JasmineRunnerArgumentsAndDeadline COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DDIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/jasmine-runner-check" -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckJasmineRunner.cmake")
    set_tests_properties(JasmineRunnerArgumentsAndDeadline PROPERTIES TIMEOUT 60)
endif()

# The publication script that archives the files in a release refuses a missing or invalid manifest (needs bash and jq).
find_program(WEBFRONT_BASH bash)
find_program(WEBFRONT_JQ jq)
if(WEBFRONT_BASH AND WEBFRONT_JQ AND NOT WIN32)
    add_test(NAME ArchiveReleaseRejectsInvalidManifest COMMAND ${CMAKE_COMMAND}
        -DSCRIPT=${PROJECT_SOURCE_DIR}/js/developer/tools/archive-release.sh -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/archive-release
        -DBASH=${WEBFRONT_BASH} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckArchiveRelease.cmake)
    set_tests_properties(ArchiveReleaseRejectsInvalidManifest PROPERTIES TIMEOUT 30)
endif()

# A browser reuses the modules it loaded, by URL, in its profile (WebKitGTK: XDG_DATA_HOME and XDG_CACHE_HOME). The first
# page that loads another version of the environment through the same profile fails with "Importing a module script
# failed", and the pinned bootstrap environment differs from the one built from the sources as soon as they are ahead of
# the pin. Each group of tests that does not go through RunWfDriver.cmake (which empties its own profile) has a profile of
# its own, emptied by the fixture WebProfileReset before the tests run.
function(webfront_use_own_profile name group)
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-profiles/${group}")
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT "XDG_DATA_HOME=${directory}/data" "XDG_CACHE_HOME=${directory}/cache"
        "WEBFRONT_TEST_CEF_PROFILE=${directory}/cef")
    set_property(TEST ${name} APPEND PROPERTY FIXTURES_REQUIRED WebProfileReset)
endfunction()
webfront_add_web_step(WebProfileReset COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-profiles"
    PROVIDES WebProfileReset)

# The browser tests need no prepared fixtures and no Node.js. The real packages of the local npm registry are downloaded
# at configure time and verified (cmake/WebFrontRegistry.cmake); the pinned bootstrap environment then builds, with wf,
# the registry (test-registry), and the environment under test from the current sources (SelfBuild*). The fixtures of
# the tests that run that environment are assembled from their outputs by the fixture DeveloperFixtures.
include(WebFrontRegistry)
set(_browser_tests_available OFF)
if(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_OPTION}
   AND CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_PLATFORMS)
    set(_browser_tests_available ON)
    webfront_require_browser_test_display("${WEBFRONT_WEBTEST_FRONTEND}")
    webfront_provision_registry(_registry_archives "${CMAKE_CURRENT_SOURCE_DIR}/registry-packages.json"
        "${CMAKE_BINARY_DIR}/registry-archives")
endif()
# A copy of the sources that the scripts of the repository run on (#315); the registry and the environment are built in it.
set(_self_project "${CMAKE_CURRENT_BINARY_DIR}/self-build-project")
set(_registry "${_self_project}/build-registry")
set(_fixtures "${CMAKE_CURRENT_BINARY_DIR}/developer-fixtures")
if(_browser_tests_available AND TARGET webfront-agent)
    # Only the test build logs HTTP to stderr; stdout remains the JSON/MCP protocol.
    target_compile_definitions(webfront-agent PRIVATE WEBFRONT_TEST_REQUEST_LOG=1)
    webfront_optional_window_test_runner(_window_test_prefix _automation_windows_available)
    add_dependencies(webtest webfront-agent)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND _automation_windows_available)
        # webfront_add_automation_test(<name> <profile> <timeout> <client script> <arguments>...): the client launches the
        # agent and the driver, both native, on the display supplied to the whole test.
        function(webfront_add_automation_test name profile timeout client)
            webfront_add_web_step(${name} TIMEOUT ${timeout} REQUIRES DeveloperFixtures
                COMMAND ${_window_test_prefix} "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/tools/${client}" ${ARGN}
                    --agent "$<TARGET_FILE:webfront-agent>" --driver "$<TARGET_FILE:webfront-wf>"
                    --environment "${_fixtures}/environment.tgz" --provision "${_fixtures}/provision.json" --registry "${_registry}")
            webfront_use_own_profile(${name} ${profile})
        endfunction()
        set(_automation_project "${PROJECT_SOURCE_DIR}/test/fixtures/automation-project")
        foreach(_pair "session|AutomationMcpWindow" "mcp-watchdog|AutomationMcpWatchdog")
            string(REPLACE "|" ";" _entry "${_pair}")
            list(GET _entry 0 _scenario)
            list(GET _entry 1 _case)
            # The session scenario closes several agents, each of which may use its own bound of about 9 s to exit.
            webfront_add_automation_test(${_case} "automation-${_scenario}" 180 AutomationClient.py --scenario "${_scenario}"
                --fixture "${_automation_project}" --work "${CMAKE_CURRENT_BINARY_DIR}/automation-${_scenario}")
        endforeach()
        # A consumer application that opted in with enableAutomation (#501), driven by an agent attached from another process.
        webfront_add_test_program(automation_consumer AutomationConsumer.cpp DRIVER WITH_WEBTEST)
        webfront_add_automation_test(AutomationConsumerWindow automation-consumer 180 AutomationClient.py --scenario consumer
            --consumer "$<TARGET_FILE:automation_consumer>" --fixture "${PROJECT_SOURCE_DIR}/test/fixtures/consumer-project"
            --work "${CMAKE_CURRENT_BINARY_DIR}/automation-consumer")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
            "import importlib.metadata; assert importlib.metadata.version('mcp') == '2.3.0'"
            RESULT_VARIABLE _sdk_available OUTPUT_QUIET ERROR_QUIET)
        if(_sdk_available EQUAL 0)
            webfront_add_automation_test(AutomationSdkWindow automation-sdk 120 AutomationSdkClient.py
                --fixture "${_automation_project}" --work "${CMAKE_CURRENT_BINARY_DIR}/automation-sdk")
        endif()
    endif()
endif()
# The Jasmine suite is a WebFront project (suite/) that webtest runs with `wf test` in the pinned bootstrap environment;
# its dependencies come from the local registry.
set(_webtest_runs OFF)
if(_browser_tests_available)
    set(_webtest_runs ON)
    webfront_add_test_program(jasmine_consumer JasmineConsumer.cpp DRIVER WITH_WEBTEST)
    # Exercise the public helper without a configure-time public network dependency beyond the pinned registry.
    set(WEBFRONT_JASMINE_ARCHIVE_DIR "${_registry_archives}")
    # Each expected failure must fail for its own reason, which the runner names: WILL_FAIL would accept any failure.
    set(_reason_failed "Jasmine did not pass \\(status: failed\\)")
    set(_reason_pending "${_reason_failed}")
    set(_reason_unicode "${_reason_failed}")
    set(_reason_report-bounds "${_reason_failed}")
    set(_reason_broken "Jasmine did not pass \\(status: error\\)")
    set(_reason_close "exited without reporting a result")
    set(_reason_hang "timed out after 5 seconds")
    # A plain failure is one of the unicode specs, and an empty suite fails by the rule of pending ones (nothing executed).
    foreach(mode pass pending broken close hang unicode report-bounds)
        set(limit 30)
        if(mode STREQUAL "hang")
            set(limit 5)
        endif()
        webfront_add_jasmine_test(NAME ConsumerJasmine-${mode} TARGET jasmine_consumer
            FRONTEND "${WEBFRONT_WEBTEST_FRONTEND}" TIMEOUT ${limit}
            SPECS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer/${mode}.spec.js"
            ARGS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer")
        if(NOT mode STREQUAL "pass")
            set_tests_properties(ConsumerJasmine-${mode} PROPERTIES PASS_REGULAR_EXPRESSION "${_reason_${mode}}")
        endif()
        set_tests_properties(ConsumerJasmine-${mode} PROPERTIES FIXTURES_SETUP ConsumerJasmineRuns)
    endforeach()
    # Native programs and temporary spaces of the specs (issue #445): the probe is the only program the owner authorizes,
    # next to a file that is not executable.
    add_dependencies(jasmine_consumer host_probe)
    set(_not_executable "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer/index.html")
    set(_reason_host-unauthorized "${_reason_failed}")
    set(_reason_host-timeout "${_reason_failed}")
    set(_reason_host-launch "${_reason_failed}")
    foreach(mode host host-denied host-unauthorized host-timeout host-launch)
        set(programs PROGRAMS "$<TARGET_FILE:host_probe>" "${_not_executable}")
        if(mode STREQUAL "host-denied")
            set(programs)
        elseif(mode STREQUAL "host-launch")
            set(programs PROGRAMS "${_not_executable}")
        endif()
        webfront_add_jasmine_test(NAME ConsumerJasmine-${mode} TARGET jasmine_consumer
            FRONTEND "${WEBFRONT_WEBTEST_FRONTEND}" TIMEOUT 60
            SPECS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer/${mode}.spec.js"
            ARGS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer"
            ${programs})
        if(mode STREQUAL "host-denied")
            # A grant in the inherited environment must not reach a test that has none.
            set_tests_properties(ConsumerJasmine-host-denied PROPERTIES ENVIRONMENT
                "WEBFRONT_JASMINE_HOST=${CMAKE_CURRENT_BINARY_DIR}/webfront-jasmine/ConsumerJasmine-host/host-$<CONFIG>.json")
        endif()
        if(DEFINED _reason_${mode})
            set_tests_properties(ConsumerJasmine-${mode} PROPERTIES PASS_REGULAR_EXPRESSION "${_reason_${mode}}")
        endif()
    endforeach()
    # Exercise the short timeout/error paths without a real stalled native operation (#504).
    set(_async_diagnostics "${CMAKE_CURRENT_SOURCE_DIR}/suite/specs/async-diagnostics.js")
    webfront_add_jasmine_test(NAME ConsumerJasmine-async-diagnostics TARGET jasmine_consumer
        FRONTEND "${WEBFRONT_WEBTEST_FRONTEND}" TIMEOUT 30
        SPECS "${_async_diagnostics}" "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer/async-diagnostics.spec.js"
        ARGS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer")
    # Window capture (issue #468) is implemented by every embedded frontend.
    set(WEBFRONT_CAPTURE_FRONTENDS WebKitGTK WKWebView WebView2 CEF)
    if(WEBFRONT_WEBTEST_FRONTEND IN_LIST WEBFRONT_CAPTURE_FRONTENDS)
        webfront_add_jasmine_test(NAME ConsumerJasmine-capture TARGET jasmine_consumer
            FRONTEND "${WEBFRONT_WEBTEST_FRONTEND}" TIMEOUT 30
            SPECS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer/capture.spec.js"
            ARGS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer")
        # With visible windows, another test can occlude this view and throttle its timed image sequence on WebKit.
        set_tests_properties(ConsumerJasmine-capture PROPERTIES PROCESSORS 4)
        # File services of the engine (issue #451), shared by every embedded frontend.
        webfront_add_jasmine_test(NAME ConsumerJasmine-file-services TARGET jasmine_consumer
            FRONTEND "${WEBFRONT_WEBTEST_FRONTEND}" TIMEOUT 60
            SPECS "${_async_diagnostics}" "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer/file-services.spec.js"
            ARGS "${CMAKE_CURRENT_SOURCE_DIR}/jasmine-consumer")
        set_tests_properties(ConsumerJasmine-capture PROPERTIES FIXTURES_SETUP ConsumerCaptureRun)
        webfront_add_web_step(ConsumerJasmineCaptures TIMEOUT 15 REQUIRES ConsumerCaptureRun COMMAND "${CMAKE_COMMAND}"
            "-DDIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/webfront-jasmine/ConsumerJasmine-capture/captures"
            -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckConsumerCaptures.cmake")
    endif()
    webfront_add_web_step(ConsumerJasmineReports TIMEOUT 15 REQUIRES ConsumerJasmineRuns COMMAND "${CMAKE_COMMAND}"
        "-DDIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/webfront-jasmine" -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckConsumerJasmineReports.cmake")
    set(_suite_project "${CMAKE_CURRENT_BINARY_DIR}/web-suite-project")
    math(EXPR _suite_timeout "${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_TIMEOUT} - 10")
    webfront_add_browser_tests(webtest "${WEBFRONT_WEBTEST_FRONTEND}" "${CMAKE_CURRENT_SOURCE_DIR}" frontend_lifecycle
        ARGUMENTS --project "${_suite_project}" --registry-dir "${_registry}" --timeout ${_suite_timeout}
                  -- test --timeout ${_suite_timeout})
    webfront_use_own_profile(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_NAME} jasmine)
    if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "CEF" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        webfront_find_xvfb(_tls_xvfb)
        set(_tls_fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tls")
        file(STRINGS "${_tls_fixture}/spki.txt" _tls_spki LIMIT_COUNT 1)
        webfront_add_test_program(webtest_tls JasmineTest.cpp WITH_WEBTEST
            DEFINITIONS WEBFRONT_TEST_TLS_FIXTURE="${_tls_fixture}" WEBFRONT_TEST_MEDIA_FILE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/seek.webm")
        target_link_libraries(webtest_tls PRIVATE WebFront_boing)
    endif()
else()
    webfront_add_browser_tests(webtest "${WEBFRONT_WEBTEST_FRONTEND}" "${CMAKE_CURRENT_SOURCE_DIR}" frontend_lifecycle SKIP_JASMINE)
endif()

# The pinned bootstrap environment is downloaded and verified at configure time (or taken from WEBFRONT_BOOTSTRAP_DIR on a
# machine without network access); webfront-wf runs wf commands without interaction from it, or from an environment passed
# explicitly (the one built from the current sources).
include(WebFrontBootstrap)
if(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_OPTION}
   AND CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_PLATFORMS)
    webfront_provision_bootstrap(WEBFRONT_BOOTSTRAP_ARCHIVE URL "${WEBFRONT_BOOTSTRAP_URL}"
        INTEGRITY "${WEBFRONT_BOOTSTRAP_INTEGRITY}" DESTINATION "${CMAKE_BINARY_DIR}/bootstrap" LOCAL_DIR "${WEBFRONT_BOOTSTRAP_DIR}")
    webfront_add_test_program(webfront-wf ../driver/WebFrontWf.cpp DRIVER DEFINITIONS
        WEBFRONT_BOOTSTRAP_ARCHIVE="${WEBFRONT_BOOTSTRAP_ARCHIVE}" WEBFRONT_BOOTSTRAP_INTEGRITY="${WEBFRONT_BOOTSTRAP_INTEGRITY}")
    # webtest runs its suite in the same bootstrap environment.
    target_compile_definitions(webtest PRIVATE
        WEBFRONT_BOOTSTRAP_ARCHIVE="${WEBFRONT_BOOTSTRAP_ARCHIVE}" WEBFRONT_BOOTSTRAP_INTEGRITY="${WEBFRONT_BOOTSTRAP_INTEGRITY}")
endif()

# Exercise actual CEF command IDs, context-menu filtering and DevTools creation in the vendored runtime.
if(WEBFRONT_EMBED_CEF AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    webfront_add_test_program(cef_content_policy CEFContentPolicy.cpp WITH_WEBTEST)
    webfront_find_xvfb(_policy_xvfb)
    if(_policy_xvfb)
        add_test(NAME CEFContentPolicy COMMAND "${_policy_xvfb}" -a $<TARGET_FILE:cef_content_policy>)
        webfront_set_browser_test_properties(CEFContentPolicy "${CMAKE_CURRENT_SOURCE_DIR}" 45)
    endif()
    # File chooser and downloads (issue #451).
    webfront_add_test_program(cef_file_services CEFFileServices.cpp WITH_WEBTEST)
    if(_policy_xvfb)
        foreach(mode download choose cancel dialogs hold)
            add_test(NAME CEFFileServices-${mode} COMMAND "${_policy_xvfb}" -a $<TARGET_FILE:cef_file_services> ${mode})
            webfront_set_browser_test_properties(CEFFileServices-${mode} "${CMAKE_CURRENT_SOURCE_DIR}" 60)
        endforeach()
    endif()
endif()

if(_browser_tests_available)
    webfront_add_test_program(developer_integration DeveloperTest.cpp)
    # The programs that open a window run under Xvfb on Linux for the frontends that need it.
    set(_developer_xvfb_prefix "")
    if(WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_XVFB_LINUX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        webfront_find_xvfb(_developer_xvfb)
        if(NOT _developer_xvfb)
            message(FATAL_ERROR "Developer browser integration requires xvfb-run on this frontend")
        endif()
        set(_developer_xvfb_prefix "${_developer_xvfb}" -a)
    endif()
    # Each part is a process with its own project, origin and visible window; CTest runs them side by side.
    foreach(_part app scripts typescript jasmine terminal workspace)
        set(_name DeveloperBrowserIntegration)
        if(NOT _part STREQUAL "app")
            set(_name DeveloperBrowserIntegration-${_part})
        endif()
        webfront_add_web_step(${_name} TIMEOUT 180 REQUIRES DeveloperFixtures PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMAND ${_developer_xvfb_prefix} $<TARGET_FILE:developer_integration>
                "${_fixtures}" "${CMAKE_CURRENT_BINARY_DIR}/developer-generated" --part=${_part})
        webfront_use_own_profile(${_name} developer-${_part})
        if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "CEF")
            set_tests_properties(${_name} PROPERTIES RESOURCE_LOCK webfront_port_9002)
        endif()
    endforeach()
    # The driver on the environment built from the current sources: statuses, output and reports reach CTest.
    if(TARGET webfront-wf)
        set(_wf_project "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-project")
        set(_wf_run "${CMAKE_COMMAND}" -DROOT=${PROJECT_SOURCE_DIR} -DDRIVER=$<TARGET_FILE:webfront-wf>
            -DPROJECT=${_wf_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work
            -DENVIRONMENT=${_fixtures}/environment.tgz -DPROVISION=${_fixtures}/provision.json
            -DREGISTRY=${_registry})
        set(_wf_run ${_developer_xvfb_prefix} ${_wf_run})
        webfront_add_web_step(WfDriverProject PROVIDES WfDriverProject COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${PROJECT_SOURCE_DIR}/test/fixtures/wf-project -DPROJECT=${_wf_project} -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareWfProject.cmake)

        # webfront_add_driver_test(<name> <expected status> <wf command words separated by |>
        #     [OUTPUT <regex>] [REPORT <regex>] [EXPECT_FILE <relative path>] [COMMAND_TIMEOUT <seconds>] [FORGE <protocol>:<api>]
        #     [MANIFEST_DIR <dir>] [EXPECT_PRESENT <paths>] [EXPECT_ABSENT <paths>] [ENVIRONMENT <archive> INTEGRITY <sri>]
        #     [FRESH_PROJECT <dir>] [PROJECT_DIR <dir>] [RENAME_ENTRY <module>] [PINNED] [NO_REGISTRY] [SLEEP_ENVIRONMENT]
        #     [TIMEOUT <seconds>] [DRIVER_TIMEOUT <seconds>] [LOCK <resource>] [OWN_FIXTURES] [REQUIRES <fixtures>...] [PROVIDES <fixture>]):
        # every wf command of the tests. The environment is the one built from the sources unless PINNED (the bootstrap
        # environment provisioned at configure time); NO_REGISTRY drops the local registry. The test requires the driver's
        # project and the developer fixtures unless OWN_FIXTURES, and locks its project unless LOCK names another resource.
        # The driver's own limit leaves CTest's TIMEOUT (120 s by default) twenty seconds to see it report.
        function(webfront_add_driver_test name status command)
            cmake_parse_arguments(ARG "PINNED;NO_REGISTRY;SLEEP_ENVIRONMENT;OWN_FIXTURES"
                "OUTPUT;REPORT;EXPECT_FILE;TIMEOUT;DRIVER_TIMEOUT;COMMAND_TIMEOUT;FORGE;ENVIRONMENT;INTEGRITY;FRESH_PROJECT;PROJECT_DIR;MANIFEST_DIR;EXPECT_PRESENT;EXPECT_ABSENT;RENAME_ENTRY;LOCK;PROVIDES"
                "REQUIRES" ${ARGN})
            set(options -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work/${name} -DEXPECT_STATUS=${status} "-DCOMMAND=${command}" -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/${name})
            foreach(option OUTPUT REPORT)
                if(ARG_${option})
                    list(APPEND options "-DEXPECT_${option}=${ARG_${option}}")
                endif()
            endforeach()
            foreach(option COMMAND_TIMEOUT FORGE ENVIRONMENT INTEGRITY FRESH_PROJECT RENAME_ENTRY EXPECT_FILE MANIFEST_DIR EXPECT_PRESENT EXPECT_ABSENT)
                if(ARG_${option})
                    list(APPEND options "-D${option}=${ARG_${option}}")
                endif()
            endforeach()
            if(ARG_PROJECT_DIR)
                # Appended after the default project of _wf_run, which it replaces.
                list(APPEND options "-DPROJECT=${ARG_PROJECT_DIR}")
            endif()
            if(ARG_SLEEP_ENVIRONMENT)
                list(APPEND options -DSLEEP_ENVIRONMENT=ON)
            endif()
            if(ARG_PINNED)
                list(APPEND options -DENVIRONMENT= -DINTEGRITY=)
            endif()
            if(ARG_NO_REGISTRY)
                list(APPEND options -DREGISTRY=)
            endif()
            if(NOT ARG_TIMEOUT)
                set(ARG_TIMEOUT 120)
            endif()
            if(NOT ARG_DRIVER_TIMEOUT)
                math(EXPR ARG_DRIVER_TIMEOUT "${ARG_TIMEOUT} - 20")
            endif()
            list(APPEND options -DTIMEOUT=${ARG_DRIVER_TIMEOUT})
            add_test(NAME ${name} COMMAND ${_wf_run} ${options} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(${name} "${CMAKE_CURRENT_SOURCE_DIR}" ${ARG_TIMEOUT})
            if(NOT ARG_LOCK)
                set(ARG_LOCK "${_wf_project}")
                if(ARG_PROJECT_DIR)
                    set(ARG_LOCK "${ARG_PROJECT_DIR}")
                elseif(ARG_FRESH_PROJECT)
                    set(ARG_LOCK "${ARG_FRESH_PROJECT}")
                endif()
            endif()
            if(NOT ARG_OWN_FIXTURES)
                list(PREPEND ARG_REQUIRES WfDriverProject DeveloperFixtures)
            endif()
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${ARG_REQUIRES}" RESOURCE_LOCK "${ARG_LOCK}")
            if(ARG_PROVIDES)
                set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${ARG_PROVIDES})
            endif()
        endfunction()

        # The fixture manifest has no jasmine-core: wf test asks for it, and wf install resolves it from the local registry.
        webfront_add_driver_test(WfDriverInstall 0 "install|jasmine-core" OUTPUT "Installed 1 packages" PROVIDES WfDriverInstalled)
        # A failing command: its status, its output and the structured report reach CTest.
        webfront_add_driver_test(WfDriverTestFails 1 "test|fails" OUTPUT "FAILED: 1 spec, 1 failure" REPORT "\"failed\":1" REQUIRES WfDriverInstalled)
        # The entry module is the one environment.json declares, not a fixed name.
        webfront_add_driver_test(WfDriverRenamedEntry 0 "list" RENAME_ENTRY runner.mjs)
        # The pinned bootstrap environment, downloaded and verified at configure time, runs a command by itself.
        webfront_add_driver_test(WfDriverPinned 0 "init" PINNED NO_REGISTRY OUTPUT "Created webfront.project.json" FRESH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-pinned")
        # The application may keep the manifest and the lock in a directory of the project: wf init, then wf install, write there
        # and leave the project root alone. An unsafe directory is a usage error.
        webfront_add_driver_test(WfDriverManifestDirectory 0 "init" MANIFEST_DIR config/wf OUTPUT "Created config/wf/webfront.project.json"
            FRESH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-manifest-dir" EXPECT_PRESENT "config/wf/webfront.project.json"
            EXPECT_ABSENT "webfront.project.json" PROVIDES WfDriverManifestDirectoryInitialized)
        webfront_add_driver_test(WfDriverManifestDirectoryInstall 0 "install|jasmine-core" MANIFEST_DIR config/wf OUTPUT "Installed 1 packages"
            PROJECT_DIR "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-manifest-dir" REQUIRES WfDriverManifestDirectoryInitialized
            EXPECT_PRESENT "config/wf/webfront-packages.lock.json" EXPECT_ABSENT "webfront-packages.lock.json|webfront.project.json")
        webfront_add_driver_test(WfDriverManifestDirectoryUnsafe 64 "list" MANIFEST_DIR ../outside OUTPUT "--manifest-dir expects a relative")
        webfront_add_driver_test(WfDriverUnknownCommand 1 "nosuchcommand" OUTPUT "nosuchcommand")
        # What a script writes to its console before it ends reaches the result of the command.
        webfront_add_driver_test(WfDriverScriptOutput 0 "run|echo|one|two" OUTPUT "echo says one two")
        # Bound browser/bootstrap startup separately from the four-second command watchdog.
        # The fixture deliberately mounts slowly and records that the command actually began.
        webfront_add_driver_test(WfDriverTimeout 71 "run|sleep" OUTPUT "time limit of the driver" DRIVER_TIMEOUT 100 COMMAND_TIMEOUT 4 EXPECT_FILE sleep.started SLEEP_ENVIRONMENT)
        # A page that blocks its script cannot close itself: the driver closes the window natively and still reports.
        webfront_add_driver_test(WfDriverBlockedPage 71 "run|block" OUTPUT "time limit of the driver" DRIVER_TIMEOUT 100 COMMAND_TIMEOUT 4 EXPECT_FILE block.started SLEEP_ENVIRONMENT)
        webfront_add_driver_test(WfDriverWrongIntegrity 70 "list" OUTPUT "Integrity check failed"
            INTEGRITY "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==")
        webfront_add_driver_test(WfDriverMissingArchive 70 "list" OUTPUT "archive not found"
            ENVIRONMENT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work/absent.tgz")
        webfront_add_driver_test(WfDriverIncompatibleApi 70 "list" OUTPUT "api version 2, this application supports 1" FORGE "1:2")
        webfront_add_driver_test(WfDriverIncompatibleProtocol 70 "list" OUTPUT "protocol version 2, this application supports 1" FORGE "2:1")
        # Usage errors are reported before an engine starts (#628): these two cases run without Xvfb, so that they do not
        # depend on a display server starting.
        webfront_add_web_step(WfDriverUsage PROPERTIES PASS_REGULAR_EXPRESSION "A wf command is required"
            COMMAND $<TARGET_FILE:webfront-wf> --project "${_wf_project}")
        webfront_add_web_step(WfDriverBadTimeout PROPERTIES PASS_REGULAR_EXPRESSION "--timeout expects whole seconds"
            COMMAND $<TARGET_FILE:webfront-wf> --project "${_wf_project}" --timeout abc -- list)

        # The developer journey (#316), from the environment built from the sources and the local registry, on one project in an
        # empty directory: wf create, init, install, check, test, build, embed; the embedded application is then compiled
        # and run from an empty directory. wf preview and wf dev need the page of the terminal: DeveloperBrowserIntegration
        # runs them, on the same engine. Each step is a case of its own, in the order of the fixtures.
        # Short: on Windows the cache of jasmine-core holds a path of 262 characters under test/web/journey-project, beyond MAX_PATH.
        set(_journey "${CMAKE_BINARY_DIR}/jp")
        function(webfront_add_journey_step name status command)
            webfront_add_driver_test(${name} ${status} "${command}" PROJECT_DIR "${_journey}" ${ARGN})
        endfunction()
        webfront_add_driver_test(JourneyCreate 0 "create|life" OUTPUT "Created ui\\." FRESH_PROJECT "${_journey}" PROVIDES JourneyCreated)
        webfront_add_journey_step(JourneyInit 0 "init" OUTPUT "Created webfront.project.json" REQUIRES JourneyCreated PROVIDES JourneyInitialized)
        # The packages of the starter: the application, its types, the type checker and the specs' runner. @types archives are
        # rooted at the package name, not at package/.
        webfront_add_journey_step(JourneyInstall 0
            "install|react@19.3.1|react-dom@19.3.1|typescript@5.9.3|@types/react@19.3.0|@types/react-dom@19.3.0|jasmine-core@7.0.2"
            OUTPUT "Installed 8 packages" REQUIRES JourneyInitialized PROVIDES JourneyInstalled)
        # The files the starter lacks.
        webfront_add_web_step(JourneyFiles REQUIRES JourneyInstalled PROVIDES JourneyConfigured COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${PROJECT_SOURCE_DIR}/test/fixtures/journey -DPROJECT=${_journey} -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareJourney.cmake)
        # The TypeScript declaration that C++ generates from a type description (#383), which ui/settings.ts imports:
        # wf check verifies the starter's module, that one and the declaration.
        # Built with webtest: the CEF job and the documented commands build only webtest and the driver.
        add_dependencies(webtest forms_declaration)
        webfront_add_web_step(JourneyDeclaration REQUIRES JourneyConfigured PROVIDES JourneyDeclared
            COMMAND $<TARGET_FILE:forms_declaration> "${_journey}/ui/generated/settings.d.ts")
        webfront_add_journey_step(JourneyCheck 0 "check" OUTPUT "No type errors in 3 project files \\(TypeScript 5\\.9\\.3\\)"
            REQUIRES JourneyDeclared PROVIDES JourneyChecked)
        webfront_add_journey_step(JourneyTest 0 "test" OUTPUT "PASSED: 2 specs" REQUIRES JourneyChecked PROVIDES JourneyTested)
        webfront_add_journey_step(JourneyBuild 0 "build" OUTPUT "Build ready" REQUIRES JourneyTested PROVIDES JourneyBuilt)
        webfront_add_journey_step(JourneyEmbed 0 "embed|LifeUI" OUTPUT "Generated generated/LifeUI.hpp" REQUIRES JourneyBuilt PROVIDES JourneyEmbedded)
        # Errors of the same project: a script outside its permissions, a cache that is not there, a lock that is out of date.
        webfront_add_journey_step(JourneyPermissionDenied 1 "run|denied" OUTPUT "Permission denied: writing ui/main\\.tsx" REQUIRES JourneyEmbedded)
        webfront_add_web_step(JourneyEmptyCache REQUIRES JourneyEmbedded PROVIDES JourneyCacheEmptied
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${_journey}/.webfront/developer/packages")
        webfront_add_journey_step(JourneyOfflineMissing 1 "restore|--offline" OUTPUT "react@19\\.3\\.1 \\(missing\\)" REQUIRES JourneyCacheEmptied PROVIDES JourneyCacheChecked)
        webfront_add_journey_step(JourneyRestore 0 "restore" OUTPUT "Restored 8 of 8 packages" REQUIRES JourneyCacheChecked PROVIDES JourneyRestored)
        webfront_add_journey_step(JourneyOfflineVerified 0 "restore|--offline" OUTPUT "Cache verified offline: 8 packages" REQUIRES JourneyRestored)

        # The application of the journey, from the header its wf embed generated.
        # The maintained Life application compiled with that header, then run from an empty directory, without a project root,
        # a bootstrap or a network. The build serializes nested builds and the browser programs of the shared build tree,
        # while independent unit tests may run.
        webfront_add_test_program(life_journey LifeEmbeddedTest.cpp EXCLUDE_FROM_ALL DEFINITIONS
            WEBFRONT_LIFE_ASSETS_HEADER="${_journey}/generated/LifeUI.hpp")
        webfront_add_web_step(JourneyEmbeddedBuild TIMEOUT 180 REQUIRES JourneyEmbedded PROVIDES JourneyEmbeddedBuild LOCK webfront_build_tree
            PROPERTIES PROCESSORS 2 COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --config $<CONFIG> --target life_journey --parallel 2)
        add_test(NAME JourneyEmbeddedRun COMMAND ${_developer_xvfb_prefix} $<TARGET_FILE:life_journey>)
        webfront_set_browser_test_properties(JourneyEmbeddedRun "${CMAKE_CURRENT_SOURCE_DIR}" 60)
        set_tests_properties(JourneyEmbeddedRun PROPERTIES FIXTURES_REQUIRED JourneyEmbeddedBuild)
        set(_journey_targets life_journey)

        # The Jasmine suite of webtest: a fresh copy of the project, its dependencies installed from the local registry,
        # then `wf test` (the WebFront*Integration case) in the pinned bootstrap environment.
        if(_webtest_runs)
            webfront_add_web_step(WebSuiteProject PROVIDES WebSuiteProject COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/suite
                -DPROJECT=${_suite_project} -DLIFE_REACT=${PROJECT_SOURCE_DIR}/examples/life/react/main.tsx
                -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareWfProject.cmake)
            webfront_add_driver_test(WebSuiteInstall 0 "install" OUTPUT "Installed [0-9]+ packages" PROJECT_DIR "${_suite_project}" PINNED
                OWN_FIXTURES REQUIRES WebSuiteProject TestRegistry PROVIDES WebSuiteInstalled)
            webfront_add_driver_test(ReactAdapterCheck 0 "check" PROJECT_DIR "${_suite_project}" PINNED OWN_FIXTURES REQUIRES WebSuiteInstalled)
            webfront_add_driver_test(ReactAdapterBuild 0 "build" PROJECT_DIR "${_suite_project}" PINNED OWN_FIXTURES REQUIRES WebSuiteInstalled
                PROVIDES ReactAdapterBuilt)
            webfront_add_web_step(ReactAdapterPages REQUIRES ReactAdapterBuilt PROVIDES ReactAdapterPages LOCK "${_suite_project}"
                COMMAND ${CMAKE_COMMAND} -DBUILD=${_suite_project}/.webfront/developer/build -DOUTPUT=${_suite_project}/pages/react-example
                -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareReactExample.cmake)
            set_property(TEST ${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_NAME} APPEND PROPERTY FIXTURES_REQUIRED WebSuiteInstalled ReactAdapterPages)
            if(TARGET webtest_tls)
                # Use synthetic begin frames so the canvas specs advance under Xvfb.
                add_test(NAME WebFrontCEFHTTPSIntegration COMMAND "${_tls_xvfb}" -a $<TARGET_FILE:webtest_tls>
                    --disable-frame-rate-limit
                    "--user-data-dir=${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-profiles/jasmine-tls/cef"
                    "--ignore-certificate-errors-spki-list=${_tls_spki}"
                    --environment "${WEBFRONT_BOOTSTRAP_ARCHIVE}" --integrity "${WEBFRONT_BOOTSTRAP_INTEGRITY}"
                    --project "${_suite_project}" --registry-dir "${_registry}" --timeout ${_suite_timeout}
                    -- test --timeout ${_suite_timeout})
                webfront_set_browser_test_properties(WebFrontCEFHTTPSIntegration "${CMAKE_CURRENT_SOURCE_DIR}" "${WEBFRONT_FRONTEND_CEF_TEST_TIMEOUT}")
                webfront_use_own_profile(WebFrontCEFHTTPSIntegration jasmine-tls)
                set_property(TEST WebFrontCEFHTTPSIntegration APPEND PROPERTY FIXTURES_REQUIRED WebSuiteInstalled ReactAdapterPages)
            endif()
        endif()

        # The environment builds itself (#315): the pinned bootstrap environment runs js/developer/scripts on a copy of the
        # sources, with the build dependencies installed from the local registry that the first of them builds. The outputs
        # stay inside that copy.
        file(READ "${PROJECT_SOURCE_DIR}/js/developer/tools/distribution.json" _distribution)
        string(JSON _self_version GET "${_distribution}" version)
        # webfront_add_self_build_test(<name> <expected status> <wf command words separated by |> <driver test options>...):
        # the project is the copy, whose manifest is in js/developer, the environment the pinned bootstrap one.
        function(webfront_add_self_build_test name status command)
            webfront_add_driver_test(${name} ${status} "${command}" PROJECT_DIR "${_self_project}" MANIFEST_DIR js/developer PINNED
                OWN_FIXTURES LOCK self-build-project ${ARGN})
        endfunction()
        # webfront_add_self_build_check(<name> <script> <requires> [<arguments>...]): a CMake script on the copy.
        function(webfront_add_self_build_check name script requires)
            webfront_add_web_step(${name} TIMEOUT 120 REQUIRES ${requires} LOCK self-build-project
                COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${PROJECT_SOURCE_DIR}/test/cmake/${script})
        endfunction()

        webfront_add_web_step(SelfBuildProject TIMEOUT 60 PROVIDES SelfBuildProject LOCK self-build-project COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project} -DARCHIVES=${_registry_archives} -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareSelfBuildProject.cmake)
        # The local npm registry: the verified archives of the real packages, kept byte for byte, and the synthetic ones.
        webfront_add_self_build_test(TestRegistry 0 "run|test-registry" OUTPUT "Script test-registry completed" NO_REGISTRY
            REQUIRES SelfBuildProject PROVIDES TestRegistry)
        webfront_add_self_build_test(SelfBuildInstall 0 "install" OUTPUT "Installed [0-9]+ packages"
            REQUIRES SelfBuildProject TestRegistry PROVIDES SelfBuildInstalled)
        webfront_add_self_build_test(SelfBuild 0 "run|build|build-distribution" OUTPUT "Script build completed"
            REQUIRES SelfBuildInstalled PROVIDES SelfBuilt)
        # The registry serves the same bytes as each optional package script.
        foreach(_package "ReactAdapter|react|React|React adapter:" "Services|services|Services|Services package:" "Pty|pty|PTY|PTY package:")
            string(REPLACE "|" ";" _package "${_package}")
            list(GET _package 0 _name)
            list(GET _package 1 _directory)
            list(GET _package 2 _label)
            list(GET _package 3 _output)
            webfront_add_self_build_test(${_name}Package 0 "run|${_directory}-package" OUTPUT "${_output}" REQUIRES SelfBuildInstalled
                PROVIDES ${_name}Packaged)
            webfront_add_web_step(${_name}PackageContents REQUIRES ${_name}Packaged COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
                -DPROJECT=${_self_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/${_directory}-package-check -DDIRECTORY=${_directory}
                -DLABEL=${_label} -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckPackage.cmake)
        endforeach()
        # Developer specs migrated by #344 remain in the browser under the repository's locked Jasmine.
        webfront_add_self_build_test(DeveloperSpecs 0 "test|--timeout|240" OUTPUT "PASSED: [0-9]+ specs" TIMEOUT 300
            REQUIRES SelfBuildInstalled)
        # The reproducibility proofs build the environment again, the second time from a refreshed bootstrap: about 45 s on a
        # hosted runner. A pull request that changes none of their inputs may leave them to the master tier (#435), and a
        # local build leaves them out unless it asks for them.
        if(WEBFRONT_TEST_REPRODUCIBILITY)
            webfront_add_self_build_test(SelfBuildRepeat 0 "run|build|build-distribution-repeat" OUTPUT "Script build completed"
                REQUIRES SelfBuilt PROVIDES SelfBuiltTwice)
        endif()
        # The packaged environment must enforce the actual project's publication permissions.
        webfront_add_self_build_test(SelfBuildOutsidePermissions 1 "run|build|elsewhere" OUTPUT "Permission denied: writing elsewhere"
            REQUIRES SelfBuilt)
        if(WEBFRONT_TEST_REPRODUCIBILITY)
            # Reproducible: two builds give the very same bytes.
            set(_self_outputs environment.tgz environment.json provision.json DeveloperReleaseData.hpp runtime.mjs compiler.mjs
                script-worker.mjs typecheck-worker.mjs esbuild.wasm xterm.css LICENSES.txt package.json app-starter.tgz life-starter.tgz)
            # A list argument passed through a function would split into separate arguments, and CMake would read only the first
            # file: $<SEMICOLON> keeps it whole until the test runs.
            string(REPLACE ";" "$<SEMICOLON>" _self_outputs "${_self_outputs}")
            webfront_add_self_build_check(SelfBuildReproducible CompareFiles.cmake SelfBuiltTwice
                -DLEFT=${_self_project}/build-distribution -DRIGHT=${_self_project}/build-distribution-repeat "-DFILES=${_self_outputs}")
            set_tests_properties(SelfBuildReproducible PROPERTIES FIXTURES_SETUP SelfReproducible)
        endif()
        # Assemble fixtures from the self-built environment, retaining #344's dependency chain.
        webfront_add_web_step(DeveloperFixtures TIMEOUT 60 REQUIRES SelfBuilt PROVIDES DeveloperFixtures LOCK self-build-project
            COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project} -DOUTPUT=${_fixtures}
            -P ${PROJECT_SOURCE_DIR}/test/cmake/AssembleDeveloperFixtures.cmake)
        if(WEBFRONT_TEST_REPRODUCIBILITY)
            # Refresh is validated separately from the two identical reproducibility builds.
            webfront_add_self_build_test(SelfBuildRefresh 0 "run|build|build-distribution|--refresh-bootstrap" OUTPUT "Script build completed"
                REQUIRES SelfReproducible PROVIDES SelfRefreshed)
            webfront_add_self_build_check(SelfBuildRefreshMatchesSources CheckSelfBuildRefresh.cmake SelfRefreshed
                -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project} -DOUTPUT=build-distribution)
        endif()
        # The Pages site keeps every version and refuses to change a published one.
        webfront_add_self_build_test(SelfBuildSite 0 "run|site|build-distribution|build-site" OUTPUT "Script site completed"
            REQUIRES SelfBuilt PROVIDES SelfStaged)
        webfront_add_self_build_test(SelfBuildSiteRepeat 0 "run|site|build-distribution|build-site" OUTPUT "Script site completed"
            REQUIRES SelfStaged PROVIDES SelfStagedTwice)
        webfront_add_self_build_check(SelfBuildSiteTamper TamperFile.cmake SelfStagedTwice
            -DFILE=${_self_project}/build-site/developer/${_self_version}/provision.json)
        set_tests_properties(SelfBuildSiteTamper PROPERTIES FIXTURES_SETUP SelfTampered)
        webfront_add_self_build_test(SelfBuildSiteRefusesChange 1 "run|site|build-distribution|build-site" OUTPUT "differs; increment the distribution version"
            REQUIRES SelfTampered)
    endif()
    if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "WKWebView")
        get_filename_component(_developer_wk_plist "${PROJECT_SOURCE_DIR}/test/cmake/WKWebViewTestInfo.plist.in" ABSOLUTE)
        foreach(target developer_integration ${_journey_targets})
            string(REPLACE "_" "-" identifier "${target}")
            set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_GUI_IDENTIFIER "org.webfront.tests.${identifier}"
                MACOSX_BUNDLE_INFO_PLIST "${_developer_wk_plist}")
        endforeach()
    endif()
endif()
