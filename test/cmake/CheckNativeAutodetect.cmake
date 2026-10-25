cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
function(configure name directory expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/native-autodetect"
        -B "${WORK_DIR}/${directory}" "-DROOT=${ROOT}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
        message(FATAL_ERROR "${name}: status ${status}\n${output}\n${error}")
    endif()
endfunction()

# All calls reuse real CMake caches, including -D overrides equal to the last detected value.
configure("initial automatic OFF" off "RESULT=OFF" -DPLATFORM=Linux -DPROBE=OFF)
configure("automatic OFF is probed again" off "RESULT=ON" -DPROBE=ON)
configure("explicit ON survives a missing dependency" off "RESULT=ON"
    -DPROBE=OFF -DWEBFRONT_EMBED_WEBKITGTK:BOOL=ON)
configure("explicit ON stays explicit" off "RESULT=ON" -DPROBE=OFF)
configure("remove the override to resume detection" off "RESULT=OFF" -UWEBFRONT_EMBED_WEBKITGTK)
configure("explicit OFF survives a newly available dependency" off "RESULT=OFF"
    -DPROBE=ON -DWEBFRONT_EMBED_WEBKITGTK:BOOL=OFF)
configure("explicit OFF stays explicit" off "RESULT=OFF" -DPROBE=ON)
configure("initial automatic ON" on "RESULT=ON" -DPLATFORM=Linux -DPROBE=ON)
configure("automatic ON is probed again" on "RESULT=OFF" -DPROBE=OFF)
configure("examples on an unsupported platform" unsupported "no native engine is registered for FreeBSD"
    -DPLATFORM=FreeBSD -DTEST_EXAMPLES=ON)
message(STATUS "Native detection preserves explicit options across configurations")
