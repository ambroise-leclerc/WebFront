add_executable(webtest JasmineTest.cpp)
target_link_libraries(webtest PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
if(MSVC)
    # The shared browser suite instantiates enough bridge callbacks to exceed standard COFF's section limit.
    target_compile_options(webtest PRIVATE /bigobj)
endif()

include(Frontends)
include("${PROJECT_SOURCE_DIR}/test/cmake/DriverSessions.cmake")
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
add_test(NAME WebProfileReset COMMAND ${CMAKE_COMMAND} -E remove_directory "${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-profiles")
set_tests_properties(WebProfileReset PROPERTIES FIXTURES_SETUP WebProfileReset LABELS "web-integration" TIMEOUT 30)

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
    webfront_optional_window_test_runner(_window_test_prefix _automation_windows_available)
    add_dependencies(webtest webfront-agent)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND _automation_windows_available)
        set(_agent_command "$<TARGET_FILE:webfront-agent>")
        # The client launches both native processes on the display supplied to the whole test.
        set(_agent_prefix "${_window_test_prefix}")
        foreach(_pair "session|AutomationMcpWindow" "mcp-watchdog|AutomationMcpWatchdog" "cli-watchdog|AutomationCliWatchdog")
            string(REPLACE "|" ";" _entry "${_pair}")
            list(GET _entry 0 _scenario)
            list(GET _entry 1 _case)
            add_test(NAME ${_case} COMMAND ${_agent_prefix} "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/test/tools/AutomationClient.py"
                --scenario "${_scenario}"
                --agent "${_agent_command}" --driver "$<TARGET_FILE:webfront-wf>"
                --environment "${_fixtures}/environment.tgz" --provision "${_fixtures}/provision.json"
                --registry "${_registry}" --fixture "${PROJECT_SOURCE_DIR}/test/fixtures/automation-project"
                --work "${CMAKE_CURRENT_BINARY_DIR}/automation-${_scenario}")
            set_tests_properties(${_case} PROPERTIES FIXTURES_REQUIRED DeveloperFixtures
                LABELS web-integration TIMEOUT 120)
            webfront_use_own_profile(${_case} "automation-${_scenario}")
            if(NOT _scenario STREQUAL "session")
                # One deliberately spinning renderer at a time, without serializing unrelated browser tests.
                set_tests_properties(${_case} PROPERTIES RESOURCE_LOCK webfront_automation_watchdog)
            endif()
        endforeach()
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c
            "import importlib.metadata; assert importlib.metadata.version('mcp') == '2.3.0'"
            RESULT_VARIABLE _sdk_available OUTPUT_QUIET ERROR_QUIET)
        if(_sdk_available EQUAL 0)
            add_test(NAME AutomationSdkWindow COMMAND ${_agent_prefix} "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/test/tools/AutomationSdkClient.py"
                --agent "${_agent_command}" --driver "$<TARGET_FILE:webfront-wf>"
                --environment "${_fixtures}/environment.tgz" --provision "${_fixtures}/provision.json"
                --registry "${_registry}" --fixture "${PROJECT_SOURCE_DIR}/test/fixtures/automation-project"
                --work "${CMAKE_CURRENT_BINARY_DIR}/automation-sdk")
            set_tests_properties(AutomationSdkWindow PROPERTIES FIXTURES_REQUIRED DeveloperFixtures
                LABELS web-integration TIMEOUT 120)
            webfront_use_own_profile(AutomationSdkWindow automation-sdk)
        endif()
    endif()
endif()
# The Jasmine suite is a WebFront project (suite/) that webtest runs with `wf test` in the pinned bootstrap environment;
# its dependencies come from the local registry.
set(_webtest_runs OFF)
if(_browser_tests_available)
    set(_webtest_runs ON)
    add_executable(jasmine_consumer JasmineConsumer.cpp)
    # The CEF CI builds webtest explicitly, then runs every web-integration case.
    add_dependencies(webtest jasmine_consumer)
    target_link_libraries(jasmine_consumer PRIVATE WebFront WebFront_test_hardening WebFront_options WebFront_warnings)
    target_compile_definitions(jasmine_consumer PRIVATE WEBFRONT_DRIVER_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE})
    webfront_setup_frontends_for_target(jasmine_consumer)
    # Exercise the public helper without a configure-time public network dependency beyond the pinned registry.
    set(WEBFRONT_JASMINE_ARCHIVE_DIR "${_registry_archives}")
    # Each expected failure must fail for its own reason, which the runner names: WILL_FAIL would accept any failure.
    set(_reason_fail "Jasmine did not pass \\(status: failed\\)")
    set(_reason_empty "${_reason_fail}")
    set(_reason_pending "${_reason_fail}")
    set(_reason_broken "Jasmine did not pass \\(status: error\\)")
    set(_reason_close "exited without reporting a result")
    set(_reason_hang "timed out after 5 seconds")
    foreach(mode pass fail empty pending broken close hang)
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
    add_test(NAME ConsumerJasmineReports COMMAND "${CMAKE_COMMAND}"
        "-DDIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/webfront-jasmine"
        -P "${PROJECT_SOURCE_DIR}/test/cmake/CheckConsumerJasmineReports.cmake")
    set_tests_properties(ConsumerJasmineReports PROPERTIES FIXTURES_REQUIRED ConsumerJasmineRuns
        LABELS web-integration TIMEOUT 15)
    set(_suite_project "${CMAKE_CURRENT_BINARY_DIR}/web-suite-project")
    math(EXPR _suite_timeout "${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_TIMEOUT} - 10")
    webfront_add_browser_tests(webtest "${WEBFRONT_WEBTEST_FRONTEND}" "${CMAKE_CURRENT_SOURCE_DIR}" frontend_lifecycle
        ARGUMENTS --project "${_suite_project}" --registry-dir "${_registry}" --timeout ${_suite_timeout}
                  -- test --timeout ${_suite_timeout})
    webfront_use_own_profile(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_NAME} jasmine)
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
    add_executable(webfront-wf ../driver/WebFrontWf.cpp)
    target_link_libraries(webfront-wf PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(webfront-wf PRIVATE WEBFRONT_DRIVER_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE}
        WEBFRONT_BOOTSTRAP_ARCHIVE="${WEBFRONT_BOOTSTRAP_ARCHIVE}" WEBFRONT_BOOTSTRAP_INTEGRITY="${WEBFRONT_BOOTSTRAP_INTEGRITY}")
    webfront_setup_frontends_for_target(webfront-wf)
    # webtest runs its suite in the same bootstrap environment.
    target_compile_definitions(webtest PRIVATE
        WEBFRONT_BOOTSTRAP_ARCHIVE="${WEBFRONT_BOOTSTRAP_ARCHIVE}" WEBFRONT_BOOTSTRAP_INTEGRITY="${WEBFRONT_BOOTSTRAP_INTEGRITY}")
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

if(_browser_tests_available)
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
    # One long script (developer-specs.mjs) would run for two minutes on its own: it is cut into parts that start from their
    # own project and run in parallel (#435). `app` also generates the header that LifeEmbeddedBuild compiles. Each part
    # listens on a port the system picks, so none of them holds webfront_port_9002.
    foreach(_part app scripts typescript jasmine terminal)
        set(_name DeveloperBrowserIntegration)
        if(NOT _part STREQUAL "app")
            set(_name DeveloperBrowserIntegration-${_part})
        endif()
        add_test(NAME ${_name} COMMAND ${_developer_command}
            "${_fixtures}" "${CMAKE_CURRENT_BINARY_DIR}/developer-generated" --part=${_part})
        set_tests_properties(${_name} PROPERTIES LABELS "web-integration" TIMEOUT 180
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" FIXTURES_REQUIRED DeveloperFixtures)
        webfront_use_own_profile(${_name} developer-${_part})
        # DeveloperTest uses CEFFrontend's default profile beside the executable, as do the other native browser
        # tests holding this lock. CEF forwards concurrent launches with that profile to an existing process.
        if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "CEF")
            set_tests_properties(${_name} PROPERTIES RESOURCE_LOCK webfront_port_9002)
        endif()
    endforeach()
    set_tests_properties(DeveloperBrowserIntegration PROPERTIES FIXTURES_SETUP DeveloperGenerated)
    # The driver on the environment built from the current sources: statuses, output and reports reach CTest.
    if(TARGET webfront-wf)
        set(_wf_project "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-project")
        set(_wf_run "${CMAKE_COMMAND}" -DROOT=${PROJECT_SOURCE_DIR} -DDRIVER=$<TARGET_FILE:webfront-wf>
            -DPROJECT=${_wf_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work
            -DENVIRONMENT=${_fixtures}/environment.tgz -DPROVISION=${_fixtures}/provision.json
            -DREGISTRY=${_registry})
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
        #     [OUTPUT <regex>] [REPORT <regex>] [EXPECT_FILE <relative path>] [DRIVER_TIMEOUT <seconds>] [COMMAND_TIMEOUT <seconds>] [FORGE <protocol>:<api>]
        #     [MANIFEST_DIR <dir>] [EXPECT_PRESENT <paths>] [EXPECT_ABSENT <paths>] [ENVIRONMENT <archive> INTEGRITY <sri>] [FRESH_PROJECT <dir>] [PROJECT_DIR <dir>] [RENAME_ENTRY <module>] [PINNED] [SLEEP_ENVIRONMENT] [REQUIRES <fixture>] [PROVIDES <fixture>])
        function(webfront_add_driver_test name status command)
            cmake_parse_arguments(ARG "PINNED;SLEEP_ENVIRONMENT" "OUTPUT;REPORT;EXPECT_FILE;DRIVER_TIMEOUT;COMMAND_TIMEOUT;FORGE;ENVIRONMENT;INTEGRITY;FRESH_PROJECT;PROJECT_DIR;MANIFEST_DIR;EXPECT_PRESENT;EXPECT_ABSENT;RENAME_ENTRY;REQUIRES;PROVIDES" "" ${ARGN})
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
                # No environment passed: the driver runs the pinned bootstrap environment provisioned at configure time.
                list(APPEND options -DENVIRONMENT= -DINTEGRITY= -DREGISTRY=)
            endif()
            if(ARG_DRIVER_TIMEOUT)
                list(APPEND options -DTIMEOUT=${ARG_DRIVER_TIMEOUT})
            endif()
            set(group "")
            if(WEBFRONT_TEST_SESSIONS)
                if(name MATCHES "^WfDriver(Install|TestPasses|TestFails|UnknownCommand|ScriptOutput|SessionWriteStorage|SessionReadStorage)$")
                    set(group WfDriverSession)
                elseif(name MATCHES "^Journey(Create|Init|Install)$")
                    set(group JourneyStartSession)
                elseif(name MATCHES "^Journey(Check|Test|Build|Embed)$")
                    set(group JourneyBuildSession)
                elseif(name MATCHES "^Journey(OfflineMissing|Restore|OfflineVerified)$")
                    set(group JourneyRestoreSession)
                endif()
            endif()
            if(group)
                set(case_timeout 100)
                if(ARG_DRIVER_TIMEOUT)
                    set(case_timeout ${ARG_DRIVER_TIMEOUT})
                endif()
                webfront_record_suite_case(${group} ${name} ${status} "${command}" ${case_timeout})
                list(APPEND options -DCASE_REPORT=${CMAKE_CURRENT_BINARY_DIR}/driver-sessions/${group}/${name}.json)
            endif()
            add_test(NAME ${name} COMMAND ${_wf_run} ${options} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(${name} "${CMAKE_CURRENT_SOURCE_DIR}" 120)
            set(project_lock "${_wf_project}")
            if(ARG_PROJECT_DIR)
                set(project_lock "${ARG_PROJECT_DIR}")
            elseif(ARG_FRESH_PROJECT)
                set(project_lock "${ARG_FRESH_PROJECT}")
            endif()
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "WfDriverProject;DeveloperFixtures;${ARG_REQUIRES};${group}"
                RESOURCE_LOCK "${project_lock}")
            if(ARG_PROVIDES)
                set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${ARG_PROVIDES})
            endif()
        endfunction()

        # The fixture manifest has no jasmine-core: wf test asks for it, and wf install resolves it from the local registry.
        webfront_add_driver_test(WfDriverInstall 0 "install|jasmine-core" OUTPUT "Installed 1 packages" PROVIDES WfDriverInstalled)
        webfront_add_driver_test(WfDriverTestPasses 0 "test|adds" OUTPUT "PASSED: 1 spec" REPORT "\"status\":\"passed\"" REQUIRES WfDriverInstalled)
        # A failing command: its status, its output and the structured report reach CTest.
        webfront_add_driver_test(WfDriverTestFails 1 "test|fails" OUTPUT "FAILED: 1 spec, 1 failure" REPORT "\"failed\":1" REQUIRES WfDriverInstalled)
        webfront_add_driver_test(WfDriverSessionWriteStorage 0 "test|writes its own storage" OUTPUT "PASSED: 1 spec"
            REQUIRES WfDriverInstalled PROVIDES WfDriverStorageWritten)
        webfront_add_driver_test(WfDriverSessionReadStorage 0 "test|starts with clean storage" OUTPUT "PASSED: 1 spec"
            REQUIRES WfDriverStorageWritten)
        # The starters pinned by the environment are served next to its archive.
        webfront_add_driver_test(WfDriverCreateStarter 0 "create|life" OUTPUT "Created"
            FRESH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-starter")
        # The entry module is the one environment.json declares, not a fixed name.
        webfront_add_driver_test(WfDriverRenamedEntry 0 "list" RENAME_ENTRY runner.mjs)
        # The pinned bootstrap environment, downloaded and verified at configure time, runs a command by itself.
        webfront_add_driver_test(WfDriverPinned 0 "init" PINNED OUTPUT "Created webfront.project.json" FRESH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-pinned")
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
        webfront_add_driver_test(WfDriverWrongIntegrity 70 "list" OUTPUT "Integrity check failed"
            INTEGRITY "sha512-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==")
        webfront_add_driver_test(WfDriverMissingArchive 70 "list" OUTPUT "archive not found"
            ENVIRONMENT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work/absent.tgz")
        webfront_add_driver_test(WfDriverIncompatibleApi 70 "list" OUTPUT "api version 2, this application supports 1" FORGE "1:2")
        webfront_add_driver_test(WfDriverIncompatibleProtocol 70 "list" OUTPUT "protocol version 2, this application supports 1" FORGE "2:1")
        add_test(NAME WfDriverUsage COMMAND ${_developer_xvfb_prefix} $<TARGET_FILE:webfront-wf> --project "${_wf_project}")
        add_test(NAME WfDriverBadTimeout COMMAND ${_developer_xvfb_prefix} $<TARGET_FILE:webfront-wf> --project "${_wf_project}" --timeout abc -- list)
        set_tests_properties(WfDriverBadTimeout PROPERTIES PASS_REGULAR_EXPRESSION "--timeout expects whole seconds" LABELS "web-integration" TIMEOUT 30)
        set_tests_properties(WfDriverUsage PROPERTIES PASS_REGULAR_EXPRESSION "A wf command is required" LABELS "web-integration" TIMEOUT 30)

        # A mismatch must fail the session without skipping later cases or losing their independent reports.
        set(_failure_suite "${CMAKE_CURRENT_BINARY_DIR}/driver-sessions/failure.json")
        file(GENERATE OUTPUT "${_failure_suite}" CONTENT [=[
[{"name":"unexpected","command":["nosuchcommand"],"expect":"0"},
 {"name":"afterFailure","command":["list"],"expect":"0"}]
]=])
        add_test(NAME WfDriverSuiteFailure COMMAND ${_wf_run} -DSUITE=${_failure_suite}
            -DREPORT_DIR=${CMAKE_CURRENT_BINARY_DIR}/driver-sessions/failure -DEXPECT_STATUS=1
            -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/suite-failure
            "-DEXPECT_OUTPUT=suite case unexpected: expected status 0, got 1" -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
        webfront_set_browser_test_properties(WfDriverSuiteFailure "${CMAKE_CURRENT_SOURCE_DIR}" 120)
        set_tests_properties(WfDriverSuiteFailure PROPERTIES FIXTURES_REQUIRED "WfDriverProject;DeveloperFixtures"
            FIXTURES_SETUP WfDriverSuiteFailure RESOURCE_LOCK "${_wf_project}")
        add_test(NAME WfDriverSuiteContinues COMMAND ${CMAKE_COMMAND}
            -DCASE_REPORT=${CMAKE_CURRENT_BINARY_DIR}/driver-sessions/failure/afterFailure.json
            -DEXPECT_STATUS=0 -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
        set_tests_properties(WfDriverSuiteContinues PROPERTIES FIXTURES_REQUIRED WfDriverSuiteFailure LABELS "web-integration" TIMEOUT 10)

        # The developer journey (#316), from the environment built from the sources and the local registry, on one project in an
        # empty directory: wf create, init, install, check, test, build, embed; the embedded application is then compiled
        # and run from an empty directory. wf preview and wf dev need the page of the terminal: DeveloperBrowserIntegration
        # runs them, on the same engine. Each step is a case of its own, in the order of the fixtures.
        # Short: on Windows the cache of jasmine-core holds a path of 262 characters under test/web/journey-project, beyond MAX_PATH.
        set(_journey "${CMAKE_BINARY_DIR}/jp")
        function(webfront_add_journey_step name status command)
            cmake_parse_arguments(ARG "" "OUTPUT;REQUIRES;PROVIDES" "" ${ARGN})
            webfront_add_driver_test(${name} ${status} "${command}" PROJECT_DIR "${_journey}" OUTPUT "${ARG_OUTPUT}"
                REQUIRES "${ARG_REQUIRES}" PROVIDES "${ARG_PROVIDES}")
        endfunction()
        # webfront_add_journey_files(<name> <fixture it requires> <fixture it provides>): the files the starter lacks.
        function(webfront_add_journey_files name requires provides)
            add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR}/test/fixtures/journey
                -DPROJECT=${_journey} -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareJourney.cmake)
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${requires}" FIXTURES_SETUP "${provides}"
                LABELS "web-integration" TIMEOUT 30)
        endfunction()
        webfront_add_driver_test(JourneyCreate 0 "create|life" OUTPUT "Created ui\\." FRESH_PROJECT "${_journey}" PROVIDES JourneyCreated)
        webfront_add_journey_step(JourneyInit 0 "init" OUTPUT "Created webfront.project.json" REQUIRES JourneyCreated PROVIDES JourneyInitialized)
        # The packages of the starter: the application, its types, the type checker and the specs' runner. @types archives are
        # rooted at the package name, not at package/.
        webfront_add_journey_step(JourneyInstall 0
            "install|react@19.3.1|react-dom@19.3.1|typescript@5.9.3|@types/react@19.3.0|@types/react-dom@19.3.0|jasmine-core@7.0.2"
            OUTPUT "Installed 8 packages" REQUIRES JourneyInitialized PROVIDES JourneyInstalled)
        webfront_add_journey_files(JourneyFiles JourneyInstalled JourneyConfigured)
        # The TypeScript declaration that C++ generates from a type description (#383), which ui/settings.ts imports:
        # wf check verifies the starter's module, that one and the declaration.
        # Built with webtest: the CEF job and the documented commands build only webtest and the driver.
        add_dependencies(webtest forms_declaration)
        add_test(NAME JourneyDeclaration COMMAND $<TARGET_FILE:forms_declaration> "${_journey}/ui/generated/settings.d.ts")
        set_tests_properties(JourneyDeclaration PROPERTIES FIXTURES_REQUIRED JourneyConfigured FIXTURES_SETUP JourneyDeclared
            LABELS "web-integration" TIMEOUT 30)
        webfront_add_journey_step(JourneyCheck 0 "check" OUTPUT "No type errors in 3 project files \\(TypeScript 5\\.9\\.3\\)"
            REQUIRES JourneyDeclared PROVIDES JourneyChecked)
        webfront_add_journey_step(JourneyTest 0 "test" OUTPUT "PASSED: 2 specs" REQUIRES JourneyChecked PROVIDES JourneyTested)
        webfront_add_journey_step(JourneyBuild 0 "build" OUTPUT "Build ready" REQUIRES JourneyTested PROVIDES JourneyBuilt)
        webfront_add_journey_step(JourneyEmbed 0 "embed|LifeUI" OUTPUT "Generated generated/LifeUI.hpp" REQUIRES JourneyBuilt PROVIDES JourneyEmbedded)
        # Errors of the same project: a script outside its permissions, a cache that is not there, a lock that is out of date.
        webfront_add_journey_step(JourneyPermissionDenied 1 "run|denied" OUTPUT "Permission denied: writing ui/main\\.tsx" REQUIRES JourneyEmbedded)
        add_test(NAME JourneyEmptyCache COMMAND ${CMAKE_COMMAND} -E remove_directory "${_journey}/.webfront/developer/packages")
        set_tests_properties(JourneyEmptyCache PROPERTIES FIXTURES_REQUIRED JourneyEmbedded FIXTURES_SETUP JourneyCacheEmptied
            LABELS "web-integration" TIMEOUT 30)
        webfront_add_journey_step(JourneyOfflineMissing 1 "restore|--offline" OUTPUT "react@19\\.3\\.1 \\(missing\\)" REQUIRES JourneyCacheEmptied PROVIDES JourneyCacheChecked)
        webfront_add_journey_step(JourneyRestore 0 "restore" OUTPUT "Restored 8 of 8 packages" REQUIRES JourneyCacheChecked PROVIDES JourneyRestored)
        webfront_add_journey_step(JourneyOfflineVerified 0 "restore|--offline" OUTPUT "Cache verified offline: 8 packages" REQUIRES JourneyRestored)

        # The application of the journey: the header that wf embed generated is compiled into the maintained Life application,
        # which then runs from an empty directory, without a project root, a bootstrap or a network.
        add_executable(life_journey EXCLUDE_FROM_ALL LifeEmbeddedTest.cpp)
        target_link_libraries(life_journey PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
        target_compile_definitions(life_journey PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE}
            WEBFRONT_LIFE_ASSETS_HEADER="${_journey}/generated/LifeUI.hpp")
        webfront_setup_frontends_for_target(life_journey)
        add_test(NAME JourneyEmbeddedBuild COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --config $<CONFIG> --target life_journey --parallel 2)
        # Serialize nested builds and browser executables sharing this output directory, while independent unit tests may run.
        set_tests_properties(JourneyEmbeddedBuild PROPERTIES FIXTURES_REQUIRED JourneyEmbedded
            FIXTURES_SETUP JourneyApplication LABELS "web-integration" TIMEOUT 180 RESOURCE_LOCK "webfront_build_tree" PROCESSORS 2)
        set(_journey_command "$<TARGET_FILE:life_journey>")
        if(_developer_xvfb)
            set(_journey_command "${_developer_xvfb}" -a ${_journey_command})
        endif()
        add_test(NAME JourneyEmbeddedRun COMMAND ${_journey_command})
        webfront_set_browser_test_properties(JourneyEmbeddedRun "${CMAKE_CURRENT_SOURCE_DIR}" 60)
        set_tests_properties(JourneyEmbeddedRun PROPERTIES FIXTURES_REQUIRED JourneyApplication)
        set(_journey_targets life_journey)

        # The Jasmine suite of webtest: a fresh copy of the project, its dependencies installed from the local registry,
        # then `wf test` (the WebFront*Integration case) in the pinned bootstrap environment.
        if(_webtest_runs)
            add_test(NAME WebSuiteProject
                COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/suite -DPROJECT=${_suite_project}
                    -DLIFE_REACT=${PROJECT_SOURCE_DIR}/examples/life/react/main.tsx
                    -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareWfProject.cmake)
            set_tests_properties(WebSuiteProject PROPERTIES FIXTURES_SETUP WebSuiteProject LABELS "web-integration" TIMEOUT 30)
            add_test(NAME WebSuiteInstall
                COMMAND ${_developer_xvfb_prefix} ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDRIVER=$<TARGET_FILE:webfront-wf>
                    -DPROJECT=${_suite_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work -DENVIRONMENT= -DINTEGRITY=
                    -DREGISTRY=${_registry} -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/WebSuiteInstall -DEXPECT_STATUS=0 -DCOMMAND=install "-DEXPECT_OUTPUT=Installed [0-9]+ packages"
                    -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(WebSuiteInstall "${CMAKE_CURRENT_SOURCE_DIR}" 120)
            set_tests_properties(WebSuiteInstall PROPERTIES FIXTURES_REQUIRED "WebSuiteProject;TestRegistry" FIXTURES_SETUP WebSuiteInstalled
                RESOURCE_LOCK "${_suite_project}")
            foreach(action Check Build)
                string(TOLOWER "${action}" command)
                add_test(NAME ReactAdapter${action}
                    COMMAND ${_developer_xvfb_prefix} ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDRIVER=$<TARGET_FILE:webfront-wf>
                        -DPROJECT=${_suite_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work -DENVIRONMENT= -DINTEGRITY=
                        -DREGISTRY=${_registry} -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/ReactAdapter${action}
                        -DEXPECT_STATUS=0 -DCOMMAND=${command} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
                webfront_set_browser_test_properties(ReactAdapter${action} "${CMAKE_CURRENT_SOURCE_DIR}" 120)
                set_tests_properties(ReactAdapter${action} PROPERTIES FIXTURES_REQUIRED WebSuiteInstalled RESOURCE_LOCK "${_suite_project}")
            endforeach()
            set_tests_properties(ReactAdapterBuild PROPERTIES FIXTURES_SETUP ReactAdapterBuilt)
            add_test(NAME ReactAdapterPages COMMAND ${CMAKE_COMMAND}
                -DBUILD=${_suite_project}/.webfront/developer/build -DOUTPUT=${_suite_project}/pages/react-example
                -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareReactExample.cmake)
            set_tests_properties(ReactAdapterPages PROPERTIES FIXTURES_REQUIRED ReactAdapterBuilt FIXTURES_SETUP ReactAdapterPages
                RESOURCE_LOCK "${_suite_project}" LABELS "web-integration" TIMEOUT 30)
            set_tests_properties(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_NAME} PROPERTIES FIXTURES_REQUIRED "WebSuiteInstalled;ReactAdapterPages")
        endif()

        # The environment builds itself (#315): the pinned bootstrap environment runs js/developer/scripts on a copy of the
        # sources, with the build dependencies installed from the local registry that the first of them builds. The outputs
        # stay inside that copy.
        file(READ "${PROJECT_SOURCE_DIR}/js/developer/tools/distribution.json" _distribution)
        string(JSON _self_version GET "${_distribution}" version)
        # webfront_add_self_build_test(<name> <expected status> <wf command words separated by |> [OUTPUT <regex>]
        #     [NO_REGISTRY] [TIMEOUT <seconds>] [REQUIRES <fixture>] [PROVIDES <fixture>]): the project is the copy, the
        #     environment the pinned bootstrap one.
        function(webfront_add_self_build_test name status command)
            cmake_parse_arguments(ARG "NO_REGISTRY" "OUTPUT;PROVIDES;TIMEOUT" "REQUIRES" ${ARGN})
            set(options -DEXPECT_STATUS=${status} "-DCOMMAND=${command}" -DPROJECT=${_self_project} -DENVIRONMENT= -DINTEGRITY=
                -DMANIFEST_DIR=js/developer -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/${name})
            if(ARG_OUTPUT)
                list(APPEND options "-DEXPECT_OUTPUT=${ARG_OUTPUT}")
            endif()
            if(ARG_NO_REGISTRY)
                list(APPEND options -DREGISTRY=)
            endif()
            set(timeout 120)
            if(ARG_TIMEOUT)
                set(timeout ${ARG_TIMEOUT})
            endif()
            # Leave time for the driver's ten-second forced-close grace before CTest kills it.
            math(EXPR driver_timeout "${timeout} - 20")
            list(APPEND options -DTIMEOUT=${driver_timeout})
            set(group "")
            if(WEBFRONT_TEST_SESSIONS AND name MATCHES "^(SelfBuildInstall|SelfBuild|DeveloperSpecs|SelfBuildRepeat|SelfBuildOutsidePermissions)$")
                set(group SelfBuildSession)
                webfront_record_suite_case(${group} ${name} ${status} "${command}" ${driver_timeout})
                list(APPEND options -DCASE_REPORT=${CMAKE_CURRENT_BINARY_DIR}/driver-sessions/${group}/${name}.json)
            endif()
            add_test(NAME ${name} COMMAND ${_wf_run} ${options} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(${name} "${CMAKE_CURRENT_SOURCE_DIR}" ${timeout})
            # The browser port stays locked (webfront_set_browser_test_properties), with the project they all write.
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${ARG_REQUIRES};${group}" RESOURCE_LOCK self-build-project)
            if(ARG_PROVIDES)
                set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${ARG_PROVIDES})
            endif()
        endfunction()
        # webfront_add_self_build_check(<name> <script> <requires> [<arguments>...]): a CMake script on the copy.
        function(webfront_add_self_build_check name script requires)
            add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${PROJECT_SOURCE_DIR}/test/cmake/${script})
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${requires}" RESOURCE_LOCK self-build-project
                LABELS "web-integration" TIMEOUT 120)
        endfunction()

        add_test(NAME SelfBuildProject COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project}
            -DARCHIVES=${_registry_archives} -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareSelfBuildProject.cmake)
        set_tests_properties(SelfBuildProject PROPERTIES FIXTURES_SETUP SelfBuildProject RESOURCE_LOCK self-build-project
            LABELS "web-integration" TIMEOUT 60)
        # The local npm registry: the verified archives of the real packages, kept byte for byte, and the synthetic ones.
        webfront_add_self_build_test(TestRegistry 0 "run|test-registry" OUTPUT "Script test-registry completed" NO_REGISTRY
            REQUIRES SelfBuildProject PROVIDES TestRegistry)
        webfront_add_self_build_test(SelfBuildInstall 0 "install" OUTPUT "Installed [0-9]+ packages"
            REQUIRES SelfBuildProject TestRegistry PROVIDES SelfBuildInstalled)
        webfront_add_self_build_test(SelfBuild 0 "run|build|build-distribution" OUTPUT "Script build completed"
            REQUIRES SelfBuildInstalled PROVIDES SelfBuilt)
        webfront_add_self_build_test(ReactAdapterPackage 0 "run|react-package" OUTPUT "React adapter:" REQUIRES SelfBuildInstalled
            PROVIDES ReactAdapterPackaged)
        add_test(NAME ReactAdapterPackageContents COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project}
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/react-package-check -P ${PROJECT_SOURCE_DIR}/test/cmake/CheckReactPackage.cmake)
        set_tests_properties(ReactAdapterPackageContents PROPERTIES FIXTURES_REQUIRED ReactAdapterPackaged LABELS "web-integration" TIMEOUT 30)
        # Developer specs migrated by #344 remain in the browser under the repository's locked Jasmine.
        webfront_add_self_build_test(DeveloperSpecs 0 "test|--timeout|240" OUTPUT "PASSED: [0-9]+ specs" TIMEOUT 300
            REQUIRES SelfBuildInstalled)
        # The reproducibility proofs build the environment again, the second time from a refreshed bootstrap: about 45 s on a
        # hosted runner. A pull request that changes none of their inputs may leave them to the master tier (#435).
        option(WEBFRONT_TEST_REPRODUCIBILITY "Build the self-built environment again, to prove it reproducible and equal to the checked-in bundles" ON)
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
                script-worker.mjs typecheck-worker.mjs esbuild.wasm xterm.css LICENSES.txt package.json life-starter.tgz)
            webfront_add_self_build_check(SelfBuildReproducible CompareFiles.cmake SelfBuiltTwice
                -DLEFT=${_self_project}/build-distribution -DRIGHT=${_self_project}/build-distribution-repeat "-DFILES=${_self_outputs}")
            set_tests_properties(SelfBuildReproducible PROPERTIES FIXTURES_SETUP SelfReproducible)
        endif()
        # Assemble fixtures from the self-built environment, retaining #344's dependency chain.
        add_test(NAME DeveloperFixtures COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project}
            -DOUTPUT=${_fixtures} -P ${PROJECT_SOURCE_DIR}/test/cmake/AssembleDeveloperFixtures.cmake)
        set_tests_properties(DeveloperFixtures PROPERTIES FIXTURES_REQUIRED SelfBuilt FIXTURES_SETUP DeveloperFixtures
            RESOURCE_LOCK self-build-project LABELS "web-integration" TIMEOUT 60)
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
    if(WEBFRONT_TEST_SESSIONS AND TARGET webfront-wf)
        # Create/reset only the root of the create/init/install session; subsequent sessions preserve the journey.
        add_test(NAME JourneySessionProject COMMAND ${CMAKE_COMMAND} -DPROJECT=${_journey}
            -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareEmptyProject.cmake)
        set_tests_properties(JourneySessionProject PROPERTIES FIXTURES_SETUP JourneySessionProject LABELS "web-integration" TIMEOUT 30)
        webfront_add_driver_suite(WfDriverSession PROJECT "${_wf_project}" REQUIRES WfDriverProject DeveloperFixtures)
        webfront_add_driver_suite(JourneyStartSession PROJECT "${_journey}" REQUIRES JourneySessionProject DeveloperFixtures)
        webfront_add_driver_suite(JourneyBuildSession PROJECT "${_journey}" REQUIRES JourneyDeclared DeveloperFixtures)
        webfront_add_driver_suite(JourneyRestoreSession PROJECT "${_journey}" REQUIRES JourneyCacheEmptied DeveloperFixtures)
        webfront_add_driver_suite(SelfBuildSession PROJECT "${_self_project}" MANIFEST_DIR js/developer PINNED
            REQUIRES SelfBuildProject TestRegistry)
    endif()
    # Compile the maintained application's actual browser-generated header, then run it without a project root.
    add_executable(life_embedded EXCLUDE_FROM_ALL LifeEmbeddedTest.cpp)
    target_link_libraries(life_embedded PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    target_compile_definitions(life_embedded PRIVATE WEBFRONT_TEST_FRONTEND=${WEBFRONT_WEBTEST_FRONTEND_TYPE}
        WEBFRONT_LIFE_ASSETS_HEADER="${CMAKE_CURRENT_BINARY_DIR}/developer-generated/LifeUI.hpp")
    webfront_setup_frontends_for_target(life_embedded)
    add_test(NAME LifeEmbeddedBuild COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
        --config $<CONFIG> --target life_embedded --parallel 2)
    # Protect the shared build tree and browser DLLs/executables, especially on Windows.
    set_tests_properties(LifeEmbeddedBuild PROPERTIES FIXTURES_REQUIRED DeveloperGenerated
        FIXTURES_SETUP LifeEmbedded LABELS "web-integration" TIMEOUT 180
        RESOURCE_LOCK "webfront_build_tree" PROCESSORS 2)
    set(_life_command "$<TARGET_FILE:life_embedded>")
    if(_developer_xvfb)
        set(_life_command "${_developer_xvfb}" -a ${_life_command})
    endif()
    add_test(NAME LifeEmbeddedIntegration COMMAND ${_life_command})
    webfront_set_browser_test_properties(LifeEmbeddedIntegration "${CMAKE_CURRENT_SOURCE_DIR}" 60)
    set_tests_properties(LifeEmbeddedIntegration PROPERTIES FIXTURES_REQUIRED LifeEmbedded)
    if(WEBFRONT_WEBTEST_FRONTEND STREQUAL "WKWebView")
        get_filename_component(_developer_wk_plist "${PROJECT_SOURCE_DIR}/test/cmake/WKWebViewTestInfo.plist.in" ABSOLUTE)
        foreach(target developer_integration life_embedded ${_journey_targets})
            string(REPLACE "_" "-" identifier "${target}")
            set_target_properties(${target} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_GUI_IDENTIFIER "org.webfront.tests.${identifier}"
                MACOSX_BUNDLE_INFO_PLIST "${_developer_wk_plist}")
        endforeach()
    endif()
endif()
