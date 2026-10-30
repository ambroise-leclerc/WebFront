# WebFront's test options (#412): the prefixed names decide, the former names are migrated only at the top level.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
# MIGRATED and IGNORED count the deprecation warnings of former names moved to, or overridden by, the prefixed ones.
function(configure name directory)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "MIGRATED;IGNORED" "EXPECT;OPTIONS")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/test-options" -B "${WORK_DIR}/${directory}"
        "-DROOT=${ROOT}" ${arg_OPTIONS}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(log "${output}${error}")
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${name}: status ${status}\n${log}")
    endif()
    foreach(expected IN LISTS arg_EXPECT)
        if(NOT log MATCHES "-- ${expected}\n")
            message(FATAL_ERROR "${name}: expected '${expected}'\n${log}")
        endif()
    endforeach()
    # CMake wraps warning text: compare it on one line.
    string(REGEX REPLACE "\n +" " " flat "${log}")
    foreach(kind MIGRATED IGNORED)
        if(kind STREQUAL "MIGRATED")
            set(pattern "ENABLE_[A-Z_]+ is deprecated and will be removed in WebFront 0\\.7\\.0: its value")
        else()
            set(pattern "ENABLE_[A-Z_]+ is deprecated and ignored")
        endif()
        string(REGEX MATCHALL "${pattern}" matches "${flat}")
        list(LENGTH matches count)
        if(NOT arg_${kind})
            set(arg_${kind} 0)
        endif()
        if(NOT count EQUAL arg_${kind})
            message(FATAL_ERROR "${name}: expected ${arg_${kind}} ${kind} warnings, got ${count}\n${log}")
        endif()
    endforeach()
endfunction()

configure("defaults" defaults
    EXPECT "TESTING=OFF LEGACY=<unset>" "FUZZING=OFF LEGACY=<unset>" "COVERAGE=OFF LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON)
configure("prefixed names at the top level" prefixed
    EXPECT "TESTING=ON LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DWEBFRONT_ENABLE_TESTING=ON -DWEBFRONT_ENABLE_FUZZING=ON -DWEBFRONT_ENABLE_COVERAGE=ON)
configure("former names at the top level are migrated" legacy MIGRATED 3
    EXPECT "TESTING=ON LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_TESTING=ON -DENABLE_FUZZING=ON -DENABLE_COVERAGE=ON)
configure("a migrated value is kept without warning" legacy
    EXPECT "TESTING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>")
configure("the prefixed name then decides alone" legacy
    EXPECT "TESTING=OFF LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>"
    OPTIONS -DWEBFRONT_ENABLE_TESTING=OFF)
configure("a former name given again is ignored once the prefixed one exists" legacy IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>"
    OPTIONS -DENABLE_TESTING=ON -DENABLE_FUZZING=OFF)
configure("the prefixed name wins when both are given" both IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_TESTING=ON -DWEBFRONT_ENABLE_TESTING=OFF)
# An old tree holds ENABLE_TESTING=ON and the FALSE/OFF defaults of the former sanitizer module.
configure("an old build tree" upgraded OPTIONS -DOLD_TREE=ON -DENABLE_TESTING=ON)
configure("an old tree's former defaults never override a new prefixed option" upgraded MIGRATED 1
    EXPECT "TESTING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>" "SANITIZER_ADDRESS=ON LEGACY=<unset>"
    OPTIONS -DOLD_TREE=OFF -DTOP_LEVEL=ON -DWEBFRONT_ENABLE_COVERAGE=ON -DWEBFRONT_ENABLE_SANITIZER_ADDRESS=ON)
configure("the former OFF defaults of an old build tree are dropped silently" old-tree
    EXPECT "TESTING=OFF LEGACY=<unset>" "SANITIZER_THREAD=OFF LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_TESTING=OFF -DENABLE_SANITIZER_THREAD=OFF)
configure("a parent's generic names are ignored" subproject
    EXPECT "TESTING=OFF LEGACY=ON" "FUZZING=OFF LEGACY=ON" "COVERAGE=OFF LEGACY=ON"
    OPTIONS -DTOP_LEVEL=OFF -DENABLE_TESTING=ON -DENABLE_FUZZING=ON -DENABLE_COVERAGE=ON)
configure("a consumer opts in with the prefixed name" consumer
    EXPECT "TESTING=ON LEGACY=OFF" "FUZZING=OFF LEGACY=ON"
    OPTIONS -DTOP_LEVEL=OFF -DENABLE_TESTING=OFF -DENABLE_FUZZING=ON -DWEBFRONT_ENABLE_TESTING=ON)
configure("former sanitizer names at the top level are migrated" sanitizers MIGRATED 5
    EXPECT "SANITIZER_ADDRESS=ON LEGACY=<unset>" "SANITIZER_LEAK=ON LEGACY=<unset>"
        "SANITIZER_UNDEFINED_BEHAVIOR=ON LEGACY=<unset>" "SANITIZER_THREAD=ON LEGACY=<unset>"
        "SANITIZER_MEMORY=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_LEAK=ON
        -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON -DENABLE_SANITIZER_THREAD=ON -DENABLE_SANITIZER_MEMORY=ON)
configure("a parent's generic sanitizer names are ignored" subproject-sanitizers
    EXPECT "SANITIZER_ADDRESS=OFF LEGACY=ON" "SANITIZER_UNDEFINED_BEHAVIOR=OFF LEGACY=ON"
    OPTIONS -DTOP_LEVEL=OFF -DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON)
message(STATUS "WebFront's test options ignore a parent's generic names and migrate the former ones")
