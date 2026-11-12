# Exercise the session coordinator's failure paths without compiling the application six times.
file(MAKE_DIRECTORY "${WORK}/fixtures")
file(WRITE "${WORK}/fixtures/provision.json" "{\"integrity\":\"sha512-unused-by-the-coordinator-spec\"}")
file(WRITE "${WORK}/fixtures/run.mjs" [=[
await webFront.ready;
const part = await webFront.cppFunction('test.part')();
// Every concurrent pane must receive rendering frames; a hidden iframe must not stand in for a shared visible window.
await new Promise(requestAnimationFrame);
if (parent !== self) {
    parent.postMessage('webfront-developer-ready', new URL(document.referrer).origin);
    await new Promise(resolve => setTimeout(resolve, 100));
}
// The first case fails. Later cases must still run; terminal deliberately reports failure too, without starting a command.
await webFront.cppFunction('test.report')(part !== 'app' && part !== 'terminal', `session-failure ${part}`);
if (parent === self) window.close();
else parent.postMessage('webfront-developer-finished', new URL(document.referrer).origin);
]=])
# Run the production fixture graph through CTest: an aggregate failure must not skip report readers.
set(helper "${CMAKE_CURRENT_LIST_DIR}/DeveloperSessionTests.cmake")
file(MAKE_DIRECTORY "${WORK}/source")
set(project [=[
cmake_minimum_required(VERSION 3.31)
project(DeveloperSessionFailure LANGUAGES NONE)
enable_testing()
include([==[@helper@]==])
set(prefix [==[@PREFIX@]==])
webfront_add_developer_session_tests(PROGRAM [==[@PROGRAM@]==] FIXTURES [==[@WORK@/fixtures]==]
    OUTPUT [==[@WORK@/output]==] PREFIX ${prefix})
set_tests_properties(DeveloperBrowserIntegration-scripts PROPERTIES FIXTURES_SETUP SuccessfulPart)
add_test(NAME SuccessfulConsumer COMMAND "${CMAKE_COMMAND}" -E echo "successful-dependent-ran")
set_tests_properties(SuccessfulConsumer PROPERTIES FIXTURES_REQUIRED SuccessfulPart)
add_test(NAME FailedConsumer COMMAND "${CMAKE_COMMAND}" -E echo "failed-dependent-must-not-run")
set_tests_properties(FailedConsumer PROPERTIES FIXTURES_REQUIRED DeveloperGenerated)
]=])
string(CONFIGURE "${project}" project @ONLY)
file(WRITE "${WORK}/source/CMakeLists.txt" "${project}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/source" -B "${WORK}/build"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot configure the CTest failure graph: ${output}${errors}")
endif()
get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST_EXECUTABLE ctest HINTS "${cmake_bin}" REQUIRED)
execute_process(COMMAND "${CTEST_EXECUTABLE}" --test-dir "${WORK}/build" -C Release --parallel 4 -V
    --output-junit "${WORK}/results.xml" -R "DeveloperBrowserIntegration|SuccessfulConsumer|FailedConsumer"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 45)
if(NOT status STREQUAL "8")
    message(FATAL_ERROR "The CTest graph must fail only for its failed cases, got ${status}: ${output}${errors}")
endif()
file(READ "${WORK}/results.xml" results)
function(expect_case name expected)
    string(FIND "${results}" "<testcase name=\"${name}\"" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "Missing CTest result for ${name}: ${results}")
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
        message(FATAL_ERROR "${name}: expected ${expected}, got ${actual}\n${item}\n${output}${errors}")
    endif()
    set(case_output "${item}" PARENT_SCOPE)
endfunction()
expect_case(DeveloperBrowserSession passed)
foreach(part app scripts typescript jasmine terminal workspace)
    set(name DeveloperBrowserIntegration)
    set(expected passed)
    if(NOT part STREQUAL "app")
        string(APPEND name "-${part}")
    endif()
    if(part STREQUAL "app" OR part STREQUAL "terminal")
        set(expected failed)
    endif()
    expect_case(${name} ${expected})
    if(NOT case_output MATCHES "session-failure ${part}")
        message(FATAL_ERROR "The individual CTest result lost the diagnostic for ${part}: ${case_output}")
    endif()
endforeach()
expect_case(SuccessfulConsumer passed)
expect_case(FailedConsumer skipped)
if(NOT output MATCHES "nativeWindows=1 parts=6")
    message(FATAL_ERROR "The session did not reach its final summary: ${output}${errors}")
endif()
