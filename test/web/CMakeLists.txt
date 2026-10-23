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
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT "XDG_DATA_HOME=${directory}/data" "XDG_CACHE_HOME=${directory}/cache")
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
    webfront_provision_registry(_registry_archives "${CMAKE_CURRENT_SOURCE_DIR}/registry-packages.json"
        "${CMAKE_BINARY_DIR}/registry-archives")
endif()
# A copy of the sources that the scripts of the repository run on (#315); the registry and the environment are built in it.
set(_self_project "${CMAKE_CURRENT_BINARY_DIR}/self-build-project")
set(_registry "${_self_project}/build-registry")
set(_fixtures "${CMAKE_CURRENT_BINARY_DIR}/developer-fixtures")
# The Jasmine suite is a WebFront project (suite/) that webtest runs with `wf test` in the pinned bootstrap environment;
# its dependencies come from the local registry.
set(_webtest_runs OFF)
if(_browser_tests_available)
    set(_webtest_runs ON)
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
    add_test(NAME DeveloperBrowserIntegration COMMAND ${_developer_command}
        "${_fixtures}" "${CMAKE_CURRENT_BINARY_DIR}/developer-generated")
    webfront_set_browser_test_properties(DeveloperBrowserIntegration "${CMAKE_CURRENT_SOURCE_DIR}" 180)
    set_tests_properties(DeveloperBrowserIntegration PROPERTIES FIXTURES_SETUP DeveloperGenerated FIXTURES_REQUIRED DeveloperFixtures)
    webfront_use_own_profile(DeveloperBrowserIntegration developer)
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
        #     [OUTPUT <regex>] [REPORT <regex>] [DRIVER_TIMEOUT <seconds>] [FORGE <protocol>:<api>]
        #     [ENVIRONMENT <archive> INTEGRITY <sri>] [FRESH_PROJECT <dir>] [RENAME_ENTRY <module>] [PINNED] [REQUIRES <fixture>] [PROVIDES <fixture>])
        function(webfront_add_driver_test name status command)
            cmake_parse_arguments(ARG "PINNED" "OUTPUT;REPORT;DRIVER_TIMEOUT;FORGE;ENVIRONMENT;INTEGRITY;FRESH_PROJECT;RENAME_ENTRY;REQUIRES;PROVIDES" "" ${ARGN})
            set(options -DEXPECT_STATUS=${status} "-DCOMMAND=${command}" -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/${name})
            foreach(option OUTPUT REPORT)
                if(ARG_${option})
                    list(APPEND options "-DEXPECT_${option}=${ARG_${option}}")
                endif()
            endforeach()
            foreach(option FORGE ENVIRONMENT INTEGRITY FRESH_PROJECT RENAME_ENTRY)
                if(ARG_${option})
                    list(APPEND options "-D${option}=${ARG_${option}}")
                endif()
            endforeach()
            if(ARG_PINNED)
                # No environment passed: the driver runs the pinned bootstrap environment provisioned at configure time.
                list(APPEND options -DENVIRONMENT= -DINTEGRITY= -DREGISTRY=)
            endif()
            if(ARG_DRIVER_TIMEOUT)
                list(APPEND options -DTIMEOUT=${ARG_DRIVER_TIMEOUT})
            endif()
            add_test(NAME ${name} COMMAND ${_wf_run} ${options} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(${name} "${CMAKE_CURRENT_SOURCE_DIR}" 120)
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "WfDriverProject;DeveloperFixtures;${ARG_REQUIRES}")
            if(ARG_PROVIDES)
                set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${ARG_PROVIDES})
            endif()
        endfunction()

        # The fixture manifest has no jasmine-core: wf test asks for it, and wf install resolves it from the local registry.
        webfront_add_driver_test(WfDriverInstall 0 "install|jasmine-core" OUTPUT "Installed 1 packages" PROVIDES WfDriverInstalled)
        webfront_add_driver_test(WfDriverTestPasses 0 "test|adds" OUTPUT "PASSED: 1 spec" REPORT "\"status\":\"passed\"" REQUIRES WfDriverInstalled)
        # A failing command: its status, its output and the structured report reach CTest.
        webfront_add_driver_test(WfDriverTestFails 1 "test|fails" OUTPUT "FAILED: 1 spec, 1 failure" REPORT "\"failed\":1" REQUIRES WfDriverInstalled)
        # The starters pinned by the environment are served next to its archive.
        webfront_add_driver_test(WfDriverCreateStarter 0 "create|pi" OUTPUT "Created"
            FRESH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-starter")
        # The entry module is the one environment.json declares, not a fixed name.
        webfront_add_driver_test(WfDriverRenamedEntry 0 "list" RENAME_ENTRY runner.mjs)
        # The pinned bootstrap environment, downloaded and verified at configure time, runs a command by itself.
        webfront_add_driver_test(WfDriverPinned 0 "init" PINNED OUTPUT "Created webfront.project.json" FRESH_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/wf-driver-pinned")
        webfront_add_driver_test(WfDriverUnknownCommand 1 "nosuchcommand" OUTPUT "nosuchcommand")
        # What a script writes to its console before it ends reaches the result of the command.
        webfront_add_driver_test(WfDriverScriptOutput 0 "run|echo|one|two" OUTPUT "echo says one two")
        webfront_add_driver_test(WfDriverTimeout 71 "run|sleep" OUTPUT "time limit of the driver" DRIVER_TIMEOUT 4)
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

        # The Jasmine suite of webtest: a fresh copy of the project, its dependencies installed from the local registry,
        # then `wf test` (the WebFront*Integration case) in the pinned bootstrap environment.
        if(_webtest_runs)
            add_test(NAME WebSuiteProject
                COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/suite -DPROJECT=${_suite_project}
                    -P ${PROJECT_SOURCE_DIR}/test/cmake/PrepareWfProject.cmake)
            set_tests_properties(WebSuiteProject PROPERTIES FIXTURES_SETUP WebSuiteProject LABELS "web-integration" TIMEOUT 30)
            add_test(NAME WebSuiteInstall
                COMMAND ${_developer_xvfb_prefix} ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDRIVER=$<TARGET_FILE:webfront-wf>
                    -DPROJECT=${_suite_project} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/wf-driver-work -DENVIRONMENT= -DINTEGRITY=
                    -DREGISTRY=${_registry} -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/WebSuiteInstall -DEXPECT_STATUS=0 -DCOMMAND=install "-DEXPECT_OUTPUT=Installed [0-9]+ packages"
                    -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            webfront_set_browser_test_properties(WebSuiteInstall "${CMAKE_CURRENT_SOURCE_DIR}" 120)
            set_tests_properties(WebSuiteInstall PROPERTIES FIXTURES_REQUIRED "WebSuiteProject;TestRegistry" FIXTURES_SETUP WebSuiteInstalled)
            set_tests_properties(${WEBFRONT_FRONTEND_${WEBFRONT_WEBTEST_FRONTEND}_TEST_NAME} PROPERTIES FIXTURES_REQUIRED WebSuiteInstalled)
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
                -DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/wf-profiles/${name})
            if(ARG_OUTPUT)
                list(APPEND options "-DEXPECT_OUTPUT=${ARG_OUTPUT}")
            endif()
            if(ARG_NO_REGISTRY)
                list(APPEND options -DREGISTRY=)
            endif()
            add_test(NAME ${name} COMMAND ${_wf_run} ${options} -P ${PROJECT_SOURCE_DIR}/test/cmake/RunWfDriver.cmake)
            set(timeout 120)
            if(ARG_TIMEOUT)
                set(timeout ${ARG_TIMEOUT})
            endif()
            webfront_set_browser_test_properties(${name} "${CMAKE_CURRENT_SOURCE_DIR}" ${timeout})
            # The browser port stays locked (webfront_set_browser_test_properties), with the project they all write.
            set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED "${ARG_REQUIRES}" RESOURCE_LOCK "webfront_port_9002;self-build-project")
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
        # The specs of the developer environment (packages, manifest, restore, scripts, wf dev, wf test, distribution): the
        # project's own `wf test`, in the browser, under the jasmine-core of its lock. They read the sources of the copy.
        webfront_add_self_build_test(DeveloperSpecs 0 "test|--timeout|240" OUTPUT "PASSED: [0-9]+ specs" TIMEOUT 300
            REQUIRES SelfBuildInstalled)
        webfront_add_self_build_test(SelfBuildRepeat 0 "run|build|build-distribution-repeat" OUTPUT "Script build completed"
            REQUIRES SelfBuilt PROVIDES SelfBuiltTwice)
        # The permissions of webfront.project.json frame where a script writes.
        webfront_add_self_build_test(SelfBuildOutsidePermissions 1 "run|build|elsewhere" OUTPUT "Permission denied: writing elsewhere"
            REQUIRES SelfBuilt)
        # Reproducible: two builds give the very same bytes.
        set(_self_outputs environment.tgz environment.json provision.json DeveloperReleaseData.hpp runtime.mjs compiler.mjs
            script-worker.mjs typecheck-worker.mjs esbuild.wasm xterm.css LICENSES.txt package.json pi-starter.tgz life-starter.tgz)
        webfront_add_self_build_check(SelfBuildReproducible CompareFiles.cmake SelfBuiltTwice
            -DLEFT=${_self_project}/build-distribution -DRIGHT=${_self_project}/build-distribution-repeat "-DFILES=${_self_outputs}")
        # The fixtures of the tests that run the environment under test: its outputs, the registry and the pages of the tests.
        add_test(NAME DeveloperFixtures COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project}
            -DOUTPUT=${_fixtures} -P ${PROJECT_SOURCE_DIR}/test/cmake/AssembleDeveloperFixtures.cmake)
        set_tests_properties(DeveloperFixtures PROPERTIES FIXTURES_REQUIRED SelfBuilt FIXTURES_SETUP DeveloperFixtures
            RESOURCE_LOCK self-build-project LABELS "web-integration" TIMEOUT 60)
        # The checked-in bootstrap bundles and checksums are rebuilt from the sources by wf, and the core headers that embed
        # them by CMake: all of them are the checked-in ones.
        webfront_add_self_build_test(SelfBuildRefresh 0 "run|build|build-distribution|--refresh-bootstrap" OUTPUT "Script build completed"
            REQUIRES SelfBuilt PROVIDES SelfRefreshed)
        webfront_add_self_build_check(SelfBuildRefreshMatchesSources CheckSelfBuildRefresh.cmake SelfRefreshed
            -DSOURCE=${PROJECT_SOURCE_DIR} -DPROJECT=${_self_project} -DOUTPUT=build-distribution)
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
