# These combinations must fail before opening a browser or starting the expensive benchmark.
foreach(mode --browser --benchmark)
    execute_process(COMMAND "${APP}" "${mode}" --capture unused.png
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT status MATCHES "^[1-9][0-9]*$" OR NOT error MATCHES "--capture requires a native window")
        message(FATAL_ERROR "${mode} --capture: expected a nonzero status and a diagnostic, got '${status}': ${output}${error}")
    endif()
endforeach()

foreach(arguments "--capture-demo" "--capture;unused.png;--capture-demo;--capture-frames;4" "--capture-frames;1" "--capture;unused.png;--capture-frames;0"
        "--capture;unused.png;--capture-frames;601" "--capture;unused.png;--capture-frames;2x")
    execute_process(COMMAND "${APP}" ${arguments}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT status STREQUAL "2" OR NOT error MATCHES "Usage:")
        message(FATAL_ERROR "${arguments}: expected usage error 2, got '${status}': ${output}${error}")
    endif()
endforeach()
