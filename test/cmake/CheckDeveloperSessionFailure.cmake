# Exercise the session coordinator's failure paths without compiling the application six times.
# Scenario "failed": parts report, some failing. Scenario "expired": the workspace part never reports and the native
# deadline (shortened for the test) ends the session, which must still leave the reports of the parts that finished.
get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST_EXECUTABLE ctest HINTS "${cmake_bin}" REQUIRED)
set(helper "${CMAKE_CURRENT_LIST_DIR}/DeveloperSessionTests.cmake")

function(run_scenario scenario)
    set(dir "${WORK}/${scenario}")
    file(MAKE_DIRECTORY "${dir}/fixtures")
    file(WRITE "${dir}/fixtures/provision.json" "{\"integrity\":\"sha512-unused-by-the-coordinator-spec\"}")
    set(hang "false")
    if(scenario STREQUAL "expired")
        set(hang "true")
    endif()
    file(WRITE "${dir}/fixtures/run.mjs" "const hang = ${hang};\n" [=[
await webFront.ready;
const part = await webFront.cppFunction('test.part')();
// Every concurrent pane must receive rendering frames; a hidden iframe must not stand in for a shared visible window.
await new Promise(requestAnimationFrame);
const host = new URLSearchParams(location.search).get('developer-session');
if (host && parent !== self) {
    parent.postMessage('webfront-developer-ready', host);
    await new Promise(resolve => setTimeout(resolve, 100));
}
if (hang && part === 'workspace') await new Promise(() => {});
// The first case fails. Later cases must still run; terminal deliberately reports failure too, without starting a command.
await webFront.cppFunction('test.report')(part !== 'app' && part !== 'terminal', `session-failure ${part}`);
if (host && parent !== self) parent.postMessage('webfront-developer-finished', host);
else window.close();
]=])
    # Run the production fixture graph through CTest: an aggregate failure must not skip report readers.
    file(MAKE_DIRECTORY "${dir}/source")
    set(project [=[
cmake_minimum_required(VERSION 3.31)
project(DeveloperSessionFailure LANGUAGES NONE)
enable_testing()
include([==[@helper@]==])
set(prefix [==[@PREFIX@]==])
webfront_add_developer_session_tests(PROGRAM [==[@PROGRAM@]==] FIXTURES [==[@dir@/fixtures]==]
    OUTPUT [==[@dir@/output]==] PREFIX ${prefix})
set_tests_properties(DeveloperBrowserIntegration-scripts PROPERTIES FIXTURES_SETUP SuccessfulPart)
add_test(NAME SuccessfulConsumer COMMAND "${CMAKE_COMMAND}" -E echo "successful-dependent-ran")
set_tests_properties(SuccessfulConsumer PROPERTIES FIXTURES_REQUIRED SuccessfulPart)
add_test(NAME FailedConsumer COMMAND "${CMAKE_COMMAND}" -E echo "failed-dependent-must-not-run")
set_tests_properties(FailedConsumer PROPERTIES FIXTURES_REQUIRED DeveloperGenerated)
]=])
    string(CONFIGURE "${project}" project @ONLY)
    file(WRITE "${dir}/source/CMakeLists.txt" "${project}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${dir}/source" -B "${dir}/build"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 20)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Cannot configure the CTest failure graph: ${output}${errors}")
    endif()
    set(config)
    if(CONFIG)
        set(config -C "${CONFIG}")
    endif()
    if(scenario STREQUAL "expired")
        set(ENV{WEBFRONT_TEST_DEVELOPER_DEADLINE} 10)
    endif()
    execute_process(COMMAND "${CTEST_EXECUTABLE}" --test-dir "${dir}/build" ${config} --parallel 4 -V
        --output-junit "${dir}/results.xml" -R "DeveloperBrowserIntegration|SuccessfulConsumer|FailedConsumer"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 120)
    unset(ENV{WEBFRONT_TEST_DEVELOPER_DEADLINE})
    if(NOT status STREQUAL "8")
        message(FATAL_ERROR "${scenario}: the CTest graph must fail only for its failed cases, got ${status}: ${output}${errors}")
    endif()
    file(READ "${dir}/results.xml" results)
    function(expect_case name expected marker)
        string(FIND "${results}" "<testcase name=\"${name}\"" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${scenario}: missing CTest result for ${name}: ${results}")
        endif()
        string(SUBSTRING "${results}" ${at} -1 item)
        string(FIND "${item}" "</testcase>" end)
        string(SUBSTRING "${item}" 0 ${end} item)
        set(actual passed)
        if(item MATCHES "<skipped")
            set(actual skipped)
        elseif(item MATCHES "<failure")
            set(actual failed)
        endif()
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "${scenario}: ${name}: expected ${expected}, got ${actual}\n${item}\n${output}${errors}")
        endif()
        if(marker AND NOT item MATCHES "${marker}")
            message(FATAL_ERROR "${scenario}: the result of ${name} lost its diagnostic '${marker}': ${item}")
        endif()
    endfunction()
    expect_case(DeveloperBrowserSession passed "")
    foreach(part app scripts typescript jasmine terminal workspace)
        set(name DeveloperBrowserIntegration)
        if(NOT part STREQUAL "app")
            string(APPEND name "-${part}")
        endif()
        set(expected passed)
        set(marker "session-failure ${part}")
        if(part STREQUAL "app" OR part STREQUAL "terminal")
            set(expected failed)
        endif()
        if(scenario STREQUAL "expired" AND part STREQUAL "workspace")
            set(expected failed)
            set(marker "did not report before the session deadline")
        endif()
        if(scenario STREQUAL "expired" AND (part STREQUAL "app" OR part STREQUAL "terminal"))
            # Their shutdown checks never ran: a reported pass cannot be claimed, a reported failure stays one.
            set(marker "session-failure ${part}")
        endif()
        expect_case(${name} ${expected} "${marker}")
    endforeach()
    expect_case(SuccessfulConsumer passed "")
    expect_case(FailedConsumer skipped "")
    if(NOT output MATCHES "nativeWindows=1 parts=6")
        message(FATAL_ERROR "${scenario}: the session did not reach its final summary: ${output}${errors}")
    endif()
    if(scenario STREQUAL "expired" AND NOT output MATCHES "aborted=1")
        message(FATAL_ERROR "The expired session did not report through its deadline path: ${output}${errors}")
    endif()
endfunction()

run_scenario(failed)
run_scenario(expired)
