# CTest owns this process. Its timeout remains outside the browser, even if JS stops responding.
file(REMOVE "${DIRECTORY}/report.json")
file(REMOVE_RECURSE "${DIRECTORY}/captures")
# The temporary spaces of the specs never survive into the next run, even if the previous one was killed.
file(REMOVE_RECURSE "${DIRECTORY}/temp")
set(host_environment "")
if(HOST)
    # The owner's native grant (webfront_add_jasmine_test PROGRAMS); without it the application reads no such file.
    set(host_environment "[==[WEBFRONT_JASMINE_HOST=${DIRECTORY}/host-${CONFIG}.json]==]")
endif()
file(READ "${DIRECTORY}/arguments-${CONFIG}.json" json)
string(JSON count LENGTH "${json}")
# Bracket arguments are literal: they keep empty strings and semicolons that a list expansion would lose.
set(arguments "")
if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last})
        string(JSON argument GET "${json}" ${index})
        string(APPEND arguments " [==[${argument}]==]")
    endforeach()
endif()
# Profile directories belong to this named test only. Never remove caller-owned directories.
file(REMOVE_RECURSE "${DIRECTORY}/profile")
set(prefix "")
if(PREFIX)
    set(prefix "[==[${PREFIX}]==] -a")
endif()
# POSIX: a wrapper kills the whole process group at the deadline (execute_process TIMEOUT only kills its direct child).
# Windows has no process group here: only the direct child is stopped, and a renderer may need manual cleanup.
math(EXPR backstop "${LIMIT} + 5")
set(wrapper "")
if(NOT CMAKE_HOST_WIN32)
    set(wrapper "[==[${CMAKE_CURRENT_LIST_DIR}/RunWithDeadline.sh]==] ${LIMIT}")
    set(wrapper "[==[sh]==] ${wrapper}")
endif()
cmake_language(EVAL CODE "execute_process(COMMAND ${wrapper} [==[${CMAKE_COMMAND}]==] -E env
    [==[WEBFRONT_JASMINE_CONFIG=${DIRECTORY}/config.json]==] ${host_environment}
    [==[XDG_DATA_HOME=${DIRECTORY}/profile/data]==] [==[XDG_CACHE_HOME=${DIRECTORY}/profile/cache]==]
    [==[WEBFRONT_TEST_CEF_PROFILE=${DIRECTORY}/profile/cef]==]
    [==[WEBFRONT_TEST_WEBVIEW2_PROFILE=${DIRECTORY}/profile/webview2]==]
    ${prefix} [==[${PROGRAM}]==]${arguments}
    RESULT_VARIABLE status TIMEOUT ${backstop})")
file(REMOVE_RECURSE "${DIRECTORY}/temp")
if(status MATCHES "timeout" OR status STREQUAL "124")
    message(FATAL_ERROR "Jasmine application timed out after ${LIMIT} seconds")
endif()
# The application exits nonzero for a failing run too: the report, when there is one, names the reason.
if(NOT EXISTS "${DIRECTORY}/report.json")
    message(FATAL_ERROR "Jasmine application exited without reporting a result (exit status: ${status})")
endif()
file(READ "${DIRECTORY}/report.json" report LIMIT 65536)
string(JSON outcome ERROR_VARIABLE error GET "${report}" status)
message(STATUS "Jasmine report: ${report}")
if(error OR NOT outcome STREQUAL "passed")
    message(FATAL_ERROR "Jasmine did not pass (status: ${outcome})")
endif()
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Jasmine application failed after a passing report: ${status}")
endif()
