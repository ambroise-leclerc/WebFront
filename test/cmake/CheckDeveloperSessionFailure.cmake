# Exercise the session coordinator's failure paths without compiling the application six times.
file(MAKE_DIRECTORY "${WORK}/fixtures")
file(WRITE "${WORK}/fixtures/provision.json" "{\"integrity\":\"sha512-unused-by-the-coordinator-spec\"}")
file(WRITE "${WORK}/fixtures/run.mjs" [=[
await webFront.ready;
const part = await webFront.cppFunction('test.part')();
// The first case fails. Later cases must still run; terminal deliberately reports failure too, without starting a command.
await webFront.cppFunction('test.report')(part !== 'app' && part !== 'terminal', `session-failure ${part}`);
parent.postMessage('webfront-developer-finished', new URL(document.referrer).origin);
]=])
execute_process(COMMAND ${PREFIX} "${PROGRAM}" "${WORK}/fixtures" "${WORK}/output" --session
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 45)
message(STATUS "${output}${errors}")
if(NOT status STREQUAL "1")
    message(FATAL_ERROR "A session with a failed first part must exit with 1, got ${status}")
endif()
foreach(part app scripts typescript jasmine terminal workspace)
    file(READ "${WORK}/output/results/${part}.json" report)
    string(JSON code GET "${report}" exitCode)
    set(expected 0)
    if(part STREQUAL "app" OR part STREQUAL "terminal")
        set(expected 1)
    endif()
    if(NOT code EQUAL expected OR NOT output MATCHES "session-failure ${part}")
        message(FATAL_ERROR "Lost or incorrect result for ${part}: ${report}")
    endif()
endforeach()
if(NOT output MATCHES "nativeWindows=1 parts=6")
    message(FATAL_ERROR "The session did not reach its final summary")
endif()
