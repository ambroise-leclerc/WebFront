# The real XDG roots reach a test through WEBFRONT_TEST_REAL_XDG_* as the environment that runs ctest has them, not as the one
# that configured the build (webfront_export_real_xdg, CaptureRealXdg.cmake), whatever the roots the test itself replaces.
# Usage: cmake -DSOURCE_DIR=<repository> -DDIRECTORY=<scratch> -P CheckRealXdg.cmake (POSIX)
set(work "${DIRECTORY}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/project")
file(WRITE "${work}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)
project(RealXdg NONE)
enable_testing()
include(\"${SOURCE_DIR}/test/cmake/WebKitGTKTestEnvironment.cmake\")
add_test(NAME Child COMMAND \"${CMAKE_COMMAND}\" \"-DOUT=${work}/seen.txt\" -P \"${SOURCE_DIR}/test/cmake/real-xdg/Child.cmake\")
webfront_configure_webkitgtk_test(Child)
")
# The configure-time environment differs from the run-time one: only the latter must reach the test.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env XDG_DATA_HOME=/configure/data XDG_CACHE_HOME=/configure/cache
    "${CMAKE_COMMAND}" -S "${work}/project" -B "${work}/build" RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Configuring the check project failed: ${error}")
endif()

# `data` and `cache` are the roots ctest runs with, none when empty; a quoted argument keeps a semicolon in it.
function(run_ctest data cache)
    file(REMOVE "${work}/seen.txt")
    set(clean --unset=XDG_DATA_HOME --unset=XDG_CACHE_HOME --unset=WEBFRONT_TEST_XDG_CAPTURED
        --unset=WEBFRONT_TEST_REAL_XDG_DATA_HOME --unset=WEBFRONT_TEST_REAL_XDG_CACHE_HOME)
    if(data)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${clean} "XDG_DATA_HOME=${data}" "XDG_CACHE_HOME=${cache}"
            "${CMAKE_CTEST_COMMAND}" --test-dir "${work}/build" --output-on-failure
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    else()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${clean}
            "${CMAKE_CTEST_COMMAND}" --test-dir "${work}/build" --output-on-failure
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    endif()
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "ctest failed: ${output}")
    endif()
    file(READ "${work}/seen.txt" seen)
    set(seen "${seen}" PARENT_SCOPE)
endfunction()

function(expect_seen expected)
    if(NOT seen STREQUAL expected)
        message(FATAL_ERROR "Expected:\n${expected}\nGot:\n${seen}")
    endif()
endfunction()

# Set at run time, with a semicolon: the test sees its throwaway roots and the real ones whole.
run_ctest("/run/a;b" "/run/cache")
# Exact lines: a root of the configure step (/configure/...) or one lost to a replacement would differ, and a scratch
# directory with spaces, brackets or the word "configure" in it is not mistaken for either.
expect_seen("XDG_DATA_HOME=[${work}/build/webkitgtk-data]\nXDG_CACHE_HOME=[${work}/build/webkitgtk-cache]\nWEBFRONT_TEST_REAL_XDG_DATA_HOME=[/run/a;b]\nWEBFRONT_TEST_REAL_XDG_CACHE_HOME=[/run/cache]\n")

# Unset at run time (but set when configuring): the real roots are absent, not the configure-time ones.
run_ctest("" "")
expect_seen("XDG_DATA_HOME=[${work}/build/webkitgtk-data]\nXDG_CACHE_HOME=[${work}/build/webkitgtk-cache]\nWEBFRONT_TEST_REAL_XDG_DATA_HOME absent\nWEBFRONT_TEST_REAL_XDG_CACHE_HOME absent\n")
