# The generic names belong to the parent (or an old build tree), not WebFront, at every project level.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
function(configure name directory)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "EXPECT;OPTIONS")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/test-options" -B "${WORK_DIR}/${directory}"
        "-DROOT=${ROOT}" ${arg_OPTIONS} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(log "${output}${error}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${name}: status ${status}\n${log}")
    endif()
    foreach(expected IN LISTS arg_EXPECT)
        if(NOT log MATCHES "-- ${expected}\n")
            message(FATAL_ERROR "${name}: expected '${expected}'\n${log}")
        endif()
    endforeach()
endfunction()
configure("defaults" defaults OPTIONS -DTOP_LEVEL=ON
    EXPECT "TESTING=OFF LEGACY=<unset>" "FUZZING=OFF LEGACY=<unset>" "COVERAGE=OFF LEGACY=<unset>")
foreach(level ON OFF)
    configure("generic options at level ${level}" "generic-${level}"
        OPTIONS -DTOP_LEVEL=${level} -DENABLE_TESTING=ON -DENABLE_FUZZING=ON -DENABLE_COVERAGE=ON
            -DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_LEAK=ON -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON
            -DENABLE_SANITIZER_THREAD=ON -DENABLE_SANITIZER_MEMORY=ON
        EXPECT "TESTING=OFF LEGACY=ON" "FUZZING=OFF LEGACY=ON" "COVERAGE=OFF LEGACY=ON"
            "SANITIZER_ADDRESS=OFF LEGACY=ON" "SANITIZER_LEAK=OFF LEGACY=ON"
            "SANITIZER_UNDEFINED_BEHAVIOR=OFF LEGACY=ON" "SANITIZER_THREAD=OFF LEGACY=ON"
            "SANITIZER_MEMORY=OFF LEGACY=ON")
    configure("explicit options at level ${level}" "generic-${level}"
        OPTIONS -DWEBFRONT_ENABLE_TESTING=ON -DWEBFRONT_ENABLE_FUZZING=ON -DWEBFRONT_ENABLE_COVERAGE=ON
            -DWEBFRONT_ENABLE_SANITIZER_ADDRESS=ON -DWEBFRONT_ENABLE_SANITIZER_LEAK=ON
            -DWEBFRONT_ENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON -DWEBFRONT_ENABLE_SANITIZER_THREAD=ON
            -DWEBFRONT_ENABLE_SANITIZER_MEMORY=ON
        EXPECT "TESTING=ON LEGACY=ON" "FUZZING=ON LEGACY=ON" "COVERAGE=ON LEGACY=ON"
            "SANITIZER_ADDRESS=ON LEGACY=ON" "SANITIZER_LEAK=ON LEGACY=ON"
            "SANITIZER_UNDEFINED_BEHAVIOR=ON LEGACY=ON" "SANITIZER_THREAD=ON LEGACY=ON"
            "SANITIZER_MEMORY=ON LEGACY=ON")
endforeach()
configure("old build tree" upgraded OPTIONS -DOLD_TREE=ON -DENABLE_TESTING=ON -DENABLE_COVERAGE=ON)
configure("old cache is not migrated" upgraded OPTIONS -DOLD_TREE=OFF -DTOP_LEVEL=ON
    EXPECT "TESTING=OFF LEGACY=ON" "COVERAGE=OFF LEGACY=ON")
configure("generic toolchain option" toolchain
    OPTIONS -DTOP_LEVEL=ON "-DCMAKE_TOOLCHAIN_FILE=${ROOT}/test/cmake/test-options/Toolchain.cmake" -DTOOLCHAIN_LEGACY=ON
    EXPECT "TESTING=OFF LEGACY=<unset>")
configure("prefixed toolchain option" toolchain-prefixed
    OPTIONS -DTOP_LEVEL=ON "-DCMAKE_TOOLCHAIN_FILE=${ROOT}/test/cmake/test-options/Toolchain.cmake" -DTOOLCHAIN_PREFIXED=ON
    EXPECT "TESTING=ON LEGACY=<unset>")
message(STATUS "WebFront options ignore generic names and old cache entries")
