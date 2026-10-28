# CTest owns this process. Its timeout remains outside the browser, even if JS stops responding.
file(REMOVE "${DIRECTORY}/report.json")
file(READ "${DIRECTORY}/arguments-${CONFIG}.json" json)
string(JSON count LENGTH "${json}")
set(arguments "")
if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last})
        string(JSON argument GET "${json}" ${index})
        string(REPLACE ";" "\\;" argument "${argument}")
        list(APPEND arguments "${argument}")
    endforeach()
endif()
# Profile directories belong to this named test only. Never remove caller-owned directories.
file(REMOVE_RECURSE "${DIRECTORY}/profile")
set(command "${PROGRAM}")
if(PREFIX)
    set(command "${PREFIX}" -a ${command})
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    "WEBFRONT_JASMINE_CONFIG=${DIRECTORY}/config.json"
    "XDG_DATA_HOME=${DIRECTORY}/profile/data" "XDG_CACHE_HOME=${DIRECTORY}/profile/cache"
    "WEBFRONT_TEST_CEF_PROFILE=${DIRECTORY}/profile/cef"
    "WEBFRONT_TEST_WEBVIEW2_PROFILE=${DIRECTORY}/profile/webview2"
    ${command} ${arguments} RESULT_VARIABLE status TIMEOUT "${LIMIT}")
if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Jasmine application failed: ${status}")
endif()
if(NOT EXISTS "${DIRECTORY}/report.json")
    message(FATAL_ERROR "Jasmine application exited without reporting a result")
endif()
file(READ "${DIRECTORY}/report.json" report LIMIT 65536)
string(JSON outcome ERROR_VARIABLE error GET "${report}" status)
message(STATUS "Jasmine report: ${report}")
if(error OR NOT outcome STREQUAL "passed")
    message(FATAL_ERROR "Jasmine did not pass")
endif()
