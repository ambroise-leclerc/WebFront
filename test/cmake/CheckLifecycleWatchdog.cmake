execute_process(COMMAND "${PROBE}" watchdog-probe RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 7)
if(NOT result STREQUAL "1" OR NOT error MATCHES "Lifecycle timeout at stage: association: waiting for second WebSocket")
    message(FATAL_ERROR "Watchdog did not report the stalled stage and fail: result=${result}, stderr=${error}")
endif()

execute_process(COMMAND "${PROBE}" watchdog-uninstrumented-probe RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 7)
if(NOT result STREQUAL "1" OR NOT error MATCHES "Lifecycle timeout at stage: uninstrumented mode or frontend initialization")
    message(FATAL_ERROR "Watchdog did not report an uninstrumented mode: result=${result}, stderr=${error}")
endif()
