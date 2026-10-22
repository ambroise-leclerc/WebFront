execute_process(
    COMMAND "${CMAKE_COMMAND}" -DWIN32=ON -DMSVC=OFF -P "${ROOT}/cmake/WebView2Support.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "requires an MSVC-compatible compiler")
    message(FATAL_ERROR "Unsupported Windows compilers must be rejected before SDK setup: ${output}${error}")
endif()
