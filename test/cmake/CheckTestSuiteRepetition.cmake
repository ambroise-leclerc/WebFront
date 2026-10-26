cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK)
    message(FATAL_ERROR "ROOT and WORK are required")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/source")
file(WRITE "${WORK}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.31)
project(StatefulTests LANGUAGES NONE)
enable_testing()
add_test(NAME Prepare COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare.cmake")
set_tests_properties(Prepare PROPERTIES FIXTURES_SETUP Fresh)
add_test(NAME Mutate COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/mutate.cmake")
set_tests_properties(Mutate PROPERTIES FIXTURES_REQUIRED Fresh)
]=])
file(WRITE "${WORK}/source/prepare.cmake" "file(WRITE state fresh)\n")
file(WRITE "${WORK}/source/mutate.cmake" [=[
file(READ state value)
if(NOT value STREQUAL "fresh")
    message(FATAL_ERROR "Fixture must be prepared before each mutation")
endif()
file(WRITE state consumed)
]=])
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/source" -B "${WORK}/build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture configure failed: ${output}${error}")
endif()
get_filename_component(cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
find_program(CTEST_EXECUTABLE ctest HINTS "${cmake_bin}" REQUIRED)
# This is the old CI command: the second mutation must fail, even though Prepare ran three times.
execute_process(COMMAND "${CTEST_EXECUTABLE}" --test-dir "${WORK}/build" -C Release --repeat until-fail:3 --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "Fixture must be prepared")
    message(FATAL_ERROR "The fixture did not expose repetition of individual mutations: ${output}${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${WORK}/build -DCONFIG=Release -DREPEAT=3 -P "${ROOT}/test/cmake/RunTestSuite.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Whole-suite repetition failed: ${output}${error}")
endif()
foreach(pass RANGE 1 3)
    if(NOT EXISTS "${WORK}/build/test-results-${pass}.xml")
        message(FATAL_ERROR "Missing JUnit report for suite pass ${pass}")
    endif()
endforeach()
# A shorter successful rerun must discard old passes without touching unrelated reports.
file(WRITE "${WORK}/build/test-results-manual.xml" "keep")
file(WRITE "${WORK}/build/test-results-2-extra.xml" "keep")
execute_process(COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${WORK}/build -DCONFIG=Release -DREPEAT=1 -P "${ROOT}/test/cmake/RunTestSuite.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR EXISTS "${WORK}/build/test-results-2.xml" OR EXISTS "${WORK}/build/test-results-3.xml"
    OR NOT EXISTS "${WORK}/build/test-results-manual.xml" OR NOT EXISTS "${WORK}/build/test-results-2-extra.xml")
    message(FATAL_ERROR "A shorter rerun must keep only its own numbered reports: ${output}${error}")
endif()
# A relative build directory (the container jobs of the CI pass "build") designates the same directory for the reports as for CTest.
file(WRITE "${WORK}/build/test-results-3.xml" "stale")
execute_process(COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=build -DCONFIG=Release -DREPEAT=2 -P "${ROOT}/test/cmake/RunTestSuite.cmake"
    WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Repetition with a relative build directory failed: ${output}${error}")
endif()
foreach(pass RANGE 1 2)
    if(NOT EXISTS "${WORK}/build/test-results-${pass}.xml")
        message(FATAL_ERROR "A relative build directory must keep the JUnit report of pass ${pass} in it")
    endif()
endforeach()
if(EXISTS "${WORK}/build/build" OR EXISTS "${WORK}/build/test-results-3.xml")
    message(FATAL_ERROR "A relative build directory must not nest its reports nor keep a stale numbered report")
endif()
# Recreate the successful reports to exercise cleanup when the next invocation fails early.
execute_process(COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${WORK}/build -DCONFIG=Release -DREPEAT=3 -P "${ROOT}/test/cmake/RunTestSuite.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Preparing reports for the failed rerun failed: ${output}${error}")
endif()
# Failures must stop the loop and propagate a nonzero process status.
file(WRITE "${WORK}/source/mutate.cmake" "message(FATAL_ERROR \"Deliberate suite failure\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${WORK}/build -DCONFIG=Release -DREPEAT=3 -P "${ROOT}/test/cmake/RunTestSuite.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR EXISTS "${WORK}/build/test-results-2.xml" OR EXISTS "${WORK}/build/test-results-3.xml"
    OR NOT "${output}${error}" MATCHES "Deliberate suite failure")
    message(FATAL_ERROR "The runner must stop at the first failed suite: ${output}${error}")
endif()

# Worker bounds are checked before CTest can accidentally choose unbounded parallelism.
foreach(parallel 0 -1 invalid)
    execute_process(COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${WORK}/build -DPARALLEL=${parallel}
        -P "${ROOT}/test/cmake/RunTestSuite.cmake" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "PARALLEL must be a positive integer")
        message(FATAL_ERROR "Invalid parallelism was not rejected: ${output}${error}")
    endif()
endforeach()

# A failed Xvfb launch is an unsuccessful invocation too: stale reports must already be gone.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    file(WRITE "${WORK}/build/test-results-2.xml" "stale")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env WEBFRONT_TEST_SHARED_DISPLAY=0
        "${CMAKE_COMMAND}" -DBUILD_DIR=${WORK}/build -DXVFB_RUN=${CMAKE_COMMAND}
        -P "${ROOT}/test/cmake/RunTestSuite.cmake" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR EXISTS "${WORK}/build/test-results-2.xml"
        OR NOT EXISTS "${WORK}/build/test-results-manual.xml")
        message(FATAL_ERROR "A failed display launch must discard only numbered stale reports: ${output}${error}")
    endif()
endif()
