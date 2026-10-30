# RunWithDeadline.sh must return when its command does, not when the deadline expires (#435), and still kill at the deadline.
function(run_timed)
    string(TIMESTAMP start "%s")
    execute_process(COMMAND sh "${SCRIPT}" ${ARGN} RESULT_VARIABLE status TIMEOUT 25)
    string(TIMESTAMP end "%s")
    math(EXPR elapsed "${end} - ${start}")
    set(status "${status}" PARENT_SCOPE)
    set(elapsed "${elapsed}" PARENT_SCOPE)
endfunction()

run_timed(20 sh -c "exit 3")
if(NOT status EQUAL 3 OR elapsed GREATER 8)
    message(FATAL_ERROR "A finished command must return its status at once: status ${status}, ${elapsed} s of a 20 s deadline")
endif()

run_timed(1 sleep 20)
if(NOT status EQUAL 124 OR elapsed GREATER 8)
    message(FATAL_ERROR "A command past its deadline must be killed: status ${status}, ${elapsed} s")
endif()
