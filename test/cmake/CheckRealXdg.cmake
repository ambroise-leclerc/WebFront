# The real XDG roots reach the programs a test's application starts, through WEBFRONT_TEST_REAL_XDG_DATA_HOME and
# _CACHE_HOME, whichever helper replaces the roots (#664, cmake/CaptureRealXdg.cmake): a test registered with
# webfront_configure_webkitgtk_test (captured by ctest from the environment of the run, not from the one that configured
# the build), the consumers' Jasmine runner (cmake/RunWebFrontJasmine.cmake), the wf driver (RunWfDriver.cmake) and the
# start-up measurement (RunStartupTime.cmake). The roots may be set (a semicolon is kept), unset, empty or mixed, and a
# capture inherited from an enclosing test wins over the throwaway roots it sees.
# Usage: cmake -DSOURCE_DIR=<repository> -DDIRECTORY=<scratch> -P CheckRealXdg.cmake (POSIX)
cmake_minimum_required(VERSION 3.25)
set(work "${DIRECTORY}")
set(program "${SOURCE_DIR}/test/cmake/real-xdg/Program.sh")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/project" "${work}/wf-project" "${work}/startup-page")
file(WRITE "${work}/project/CMakeLists.txt" "cmake_minimum_required(VERSION 3.25)
project(RealXdg NONE)
enable_testing()
include(\"${SOURCE_DIR}/test/cmake/WebKitGTKTestEnvironment.cmake\")
add_test(NAME Child COMMAND sh \"${program}\")
webfront_configure_webkitgtk_test(Child)
")
# The configure-time environment differs from the run-time one: only the latter must reach the test.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env XDG_DATA_HOME=/configure/data XDG_CACHE_HOME=/configure/cache
    "${CMAKE_COMMAND}" -S "${work}/project" -B "${work}/build" RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Configuring the check project failed: ${error}")
endif()

# The helpers, each as bracket arguments, and the throwaway roots it gives the program.
set(helper_ctest "[==[${CMAKE_CTEST_COMMAND}]==] --test-dir [==[${work}/build]==] --output-on-failure")
set(throwaway_ctest "${work}/build/webkitgtk-")
set(helper_jasmine "[==[XDG_REPORT=${work}/jasmine]==] [==[${CMAKE_COMMAND}]==] [==[-DPROGRAM=${program}]==]
    [==[-DDIRECTORY=${work}/jasmine]==] -DCONFIG=Test -DPREFIX= -DLIMIT=20
    -P [==[${SOURCE_DIR}/cmake/RunWebFrontJasmine.cmake]==]")
set(throwaway_jasmine "${work}/jasmine/profile/")
file(MAKE_DIRECTORY "${work}/jasmine")
file(WRITE "${work}/jasmine/arguments-Test.json" "[]\n")
set(helper_wf "[==[${CMAKE_COMMAND}]==] [==[-DDRIVER=${program}]==] [==[-DPROJECT=${work}/wf-project]==]
    -DEXPECT_STATUS=0 -DCOMMAND=check [==[-DPROFILE=${work}/wf-profile]==] -P [==[${SOURCE_DIR}/test/cmake/RunWfDriver.cmake]==]")
set(throwaway_wf "${work}/wf-profile/")
set(helper_startup "[==[${CMAKE_COMMAND}]==] [==[-DPROGRAM=sh;${program}]==] [==[-DWORK_DIRECTORY=${work}/startup-page]==]
    [==[-DSCRATCH=${work}/startup]==] -DTEMPERATURE=cold -DLIMIT_MS=10000
    -P [==[${SOURCE_DIR}/test/cmake/RunStartupTime.cmake]==]")
set(throwaway_startup "${work}/startup/")

# Runs `helper` in an environment cleared of the roots and of any capture, where XDG_DATA_HOME is `data` and
# XDG_CACHE_HOME is `cache` (UNSET: not defined; an empty string defines it empty), plus the assignments of ARGN.
# Reads what the program saw into `seen`.
function(run helper data cache)
    file(REMOVE "${work}/seen.txt")
    set(assignments "")
    foreach(kind DATA CACHE)
        string(TOLOWER "${kind}" parameter)
        set(value "${${parameter}}")
        if(NOT value STREQUAL "UNSET")
            string(APPEND assignments " [==[XDG_${kind}_HOME=${value}]==]")
        endif()
    endforeach()
    foreach(assignment IN LISTS ARGN)
        string(APPEND assignments " [==[${assignment}]==]")
    endforeach()
    cmake_language(EVAL CODE "execute_process(COMMAND [==[${CMAKE_COMMAND}]==] -E env --unset=XDG_DATA_HOME
        --unset=XDG_CACHE_HOME --unset=WEBFRONT_TEST_XDG_CAPTURED --unset=WEBFRONT_TEST_REAL_XDG_DATA_HOME
        --unset=WEBFRONT_TEST_REAL_XDG_CACHE_HOME [==[XDG_SEEN=${work}/seen.txt]==]${assignments} ${helper_${helper}}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "The ${helper} helper failed: ${output}")
    endif()
    if(NOT EXISTS "${work}/seen.txt")
        message(FATAL_ERROR "The ${helper} helper did not start the program: ${output}")
    endif()
    file(READ "${work}/seen.txt" seen)
    set(seen "${seen}" PARENT_SCOPE)
endfunction()

# Exact lines: a root of the configure step (/configure/...) or one lost to a replacement would differ, and a scratch
# directory with spaces, brackets or the word "configure" in it is not mistaken for either. ABSENT: no such variable.
function(expect helper real_data real_cache)
    set(expected "XDG_DATA_HOME=[${throwaway_${helper}}data]\nXDG_CACHE_HOME=[${throwaway_${helper}}cache]\n")
    foreach(kind DATA CACHE)
        string(TOLOWER "real_${kind}" parameter)
        set(value "${${parameter}}")
        if(value STREQUAL "ABSENT")
            string(APPEND expected "WEBFRONT_TEST_REAL_XDG_${kind}_HOME absent\n")
        else()
            string(APPEND expected "WEBFRONT_TEST_REAL_XDG_${kind}_HOME=[${value}]\n")
        endif()
    endforeach()
    if(NOT seen STREQUAL expected)
        message(FATAL_ERROR "The ${helper} helper. Expected:\n${expected}\nGot:\n${seen}")
    endif()
endfunction()

foreach(helper ctest jasmine wf startup)
    # Set, with a semicolon: the program sees its throwaway roots and the real ones whole.
    run(${helper} "/run/a;b" "/run/cache")
    expect(${helper} "/run/a;b" "/run/cache")
    # Unset (ctest: but set when configuring): the real roots are absent, not the configure-time ones.
    run(${helper} UNSET UNSET)
    expect(${helper} ABSENT ABSENT)
    # Mixed, one of them empty: an empty root is absent, the other is kept.
    run(${helper} "" "/run/cache")
    expect(${helper} ABSENT "/run/cache")
    # Inside a test that already captured them: its capture wins over the throwaway roots this run inherits.
    run(${helper} "/throwaway/data" "/throwaway/cache" WEBFRONT_TEST_XDG_CAPTURED=1 WEBFRONT_TEST_REAL_XDG_DATA_HOME=/kept)
    expect(${helper} "/kept" ABSENT)
endforeach()
