# Drives cmake/RunWebFrontJasmine.cmake with stand-in programs (POSIX only).
set(runner "${SOURCE_DIR}/cmake/RunWebFrontJasmine.cmake")
set(work "${DIRECTORY}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

function(run_runner program limit)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "OUT=${work}"
        "${CMAKE_COMMAND}" "-DPROGRAM=${program}" "-DDIRECTORY=${work}" -DCONFIG=Test -DPREFIX= "-DLIMIT=${limit}"
        -P "${runner}" RESULT_VARIABLE status ERROR_VARIABLE error OUTPUT_QUIET)
    set(status "${status}" PARENT_SCOPE)
    set(error "${error}" PARENT_SCOPE)
endfunction()

# Arguments arrive literally: empty, with a semicolon, with spaces and with a generator-expression lookalike.
file(WRITE "${work}/arguments-Test.json" "[\"\", \"a;b\", \"with space\", \"$<TARGET_FILE:x>\", \"last\"]\n")
run_runner("${SOURCE_DIR}/test/cmake/jasmine-runner/argv.sh" 20)
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Runner failed: ${status} ${error}")
endif()
file(READ "${work}/argv.txt" argv)
set(expected "[]\n[a;b]\n[with space]\n[$<TARGET_FILE:x>]\n[last]\n")
if(NOT argv STREQUAL expected)
    message(FATAL_ERROR "Arguments were altered. Expected:\n${expected}\nGot:\n${argv}")
endif()

# At the deadline the program and everything it started are gone.
file(WRITE "${work}/arguments-Test.json" "[]\n")
run_runner("${SOURCE_DIR}/test/cmake/jasmine-runner/tree.sh" 1)
if(status STREQUAL "0" OR NOT error MATCHES "timed out after 1 seconds")
    message(FATAL_ERROR "The runner did not report the timeout: ${status} ${error}")
endif()
file(READ "${work}/pids.txt" pids)
string(STRIP "${pids}" pids)
separate_arguments(pids)
foreach(pid IN LISTS pids)
    # The group's SIGKILL is sent before the runner returns, but an orphan may take a moment to exit on a loaded
    # machine: only one still running after 5 s survived the deadline.
    foreach(attempt RANGE 1 50)
        execute_process(COMMAND kill -0 ${pid} RESULT_VARIABLE alive OUTPUT_QUIET ERROR_QUIET)
        # A killed orphan stays a zombie when the container's PID 1 does not reap it: it holds nothing and is dead.
        # The process may also be reaped between the two checks: a state that cannot be read means it is gone. Linux
        # reads it from /proc, which a minimal container image has without ps; macOS has no /proc and asks ps.
        if(alive STREQUAL "0")
            if(EXISTS "/proc/self/stat")
                execute_process(COMMAND cat "/proc/${pid}/stat" OUTPUT_VARIABLE stat RESULT_VARIABLE unreadable ERROR_QUIET)
                set(zombie "\\) Z ")
            else()
                execute_process(COMMAND ps -o stat= -p ${pid} OUTPUT_VARIABLE stat RESULT_VARIABLE unreadable ERROR_QUIET)
                set(zombie "^[ \t]*Z")
            endif()
            if(NOT unreadable STREQUAL "0" OR stat MATCHES "${zombie}")
                set(alive 1)
            endif()
        endif()
        if(NOT alive STREQUAL "0" OR attempt EQUAL 50)
            break()
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
    endforeach()
    if(alive STREQUAL "0")
        execute_process(COMMAND kill -KILL ${pid})
        message(FATAL_ERROR "Process ${pid} survived the deadline")
    endif()
endforeach()

# A SIGKILL that is not the deadline keeps its own status instead of being reported as a timeout.
file(WRITE "${work}/arguments-Test.json" "[]\n")
run_runner("${SOURCE_DIR}/test/cmake/jasmine-runner/kill.sh" 20)
if(error MATCHES "timed out")
    message(FATAL_ERROR "An unrelated SIGKILL was reported as a timeout: ${status} ${error}")
endif()
