# Start-up time of an embedded native window (#611). Runs the lifecycle program's `startup-time` scenario with
# WEBFRONT_STARTUP_TRACE=1 and fails when the bridge takes longer than a generous limit to become usable.
#
#   -DPROGRAM=<command;with;arguments>  the lifecycle program, with its display wrapper where one is needed
#   -DWORK_DIRECTORY=<directory>        the directory the scenario's page is served from
#   -DSCRATCH=<directory>               where the fresh profile directories are created
#   -DTEMPERATURE=cold|warm             cold: first start with empty cache, data and config directories (so also an empty
#                                       GStreamer registry); warm: the measured start follows an identical one
#   -DLIMIT_MS=<number>                 the limit on the time from the application's construction to the page's answer
#
# The limit is not the project's target (500 ms warm, 1 s cold, measured on a reference machine): it only catches a
# regression of seconds on a loaded CI runner.
foreach(variable PROGRAM WORK_DIRECTORY SCRATCH TEMPERATURE LIMIT_MS)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()
if(NOT TEMPERATURE MATCHES "^(cold|warm)$")
    message(FATAL_ERROR "TEMPERATURE must be cold or warm, not '${TEMPERATURE}'")
endif()

file(REMOVE_RECURSE "${SCRATCH}")
foreach(directory home cache data config)
    file(MAKE_DIRECTORY "${SCRATCH}/${directory}")
endforeach()

set(environment
    "WEBFRONT_STARTUP_TRACE=1"
    "HOME=${SCRATCH}/home"
    "XDG_CACHE_HOME=${SCRATCH}/cache"
    "XDG_DATA_HOME=${SCRATCH}/data"
    "XDG_CONFIG_HOME=${SCRATCH}/config")
if(WIN32)
    list(APPEND environment "LOCALAPPDATA=${SCRATCH}/data" "APPDATA=${SCRATCH}/config")
endif()

function(start_once label)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${environment} ${PROGRAM} startup-time
        WORKING_DIRECTORY "${WORK_DIRECTORY}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
        TIMEOUT 60)
    message(STATUS "${label}: exit ${result}\n${output}${error}")
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "The ${label} start failed: ${result}")
    endif()
    if(NOT output MATCHES "startup-time: ready ([0-9]+) ms")
        message(FATAL_ERROR "The ${label} start did not report its time:\n${output}")
    endif()
    set(ready "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(shown "unknown" PARENT_SCOPE)
    if(error MATCHES "WebFront startup: window shown at ([0-9]+) ms")
        set(shown "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endif()
endfunction()

if(TEMPERATURE STREQUAL "warm")
    start_once("priming")  # fills the profile, the shader, font and plugin caches the measured start finds
endif()
start_once("${TEMPERATURE}")
message(STATUS "${TEMPERATURE} start: window shown ${shown} ms after exec, page answered ${ready} ms after construction (limit ${LIMIT_MS} ms)")
if(ready GREATER LIMIT_MS)
    message(FATAL_ERROR "The ${TEMPERATURE} start took ${ready} ms, over the ${LIMIT_MS} ms limit")
endif()
