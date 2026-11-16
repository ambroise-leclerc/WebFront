# These combinations must fail before opening a browser or starting the expensive benchmark.
foreach(mode --browser --benchmark)
    execute_process(COMMAND "${APP}" "${mode}" --capture unused.png
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT status MATCHES "^[1-9][0-9]*$" OR NOT error MATCHES "--capture requires a native window")
        message(FATAL_ERROR "${mode} --capture: expected a nonzero status and a diagnostic, got '${status}': ${output}${error}")
    endif()
endforeach()
