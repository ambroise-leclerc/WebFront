# Shared CTest query used by the selection suite and its focused generator fixture.
function(webfront_read_frontend_tests directory out)
    # Multi-config generators need an explicit configuration even for show-only queries.
    execute_process(
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${directory}" -C Release --show-only=json-v1
        RESULT_VARIABLE result OUTPUT_VARIABLE tests ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "CTest query for '${directory}' failed: ${error}")
    endif()
    set(${out} "${tests}" PARENT_SCOPE)
endfunction()
