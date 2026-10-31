# WebFront's test options (#412): the prefixed names decide, the former names are migrated only at the top level.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
# MIGRATED, KEPT and IGNORED count the messages of former names moved to the prefixed ones (deprecation), repeating a
# prefixed value (deprecation) and overridden by a prefixed one (a warning, which -Wno-deprecated does not hide).
function(configure name directory)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "MIGRATED;KEPT;IGNORED" "EXPECT;OPTIONS")
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
    # CMake wraps warning text: compare it with single spaces.
    string(REGEX REPLACE "[\n ]+" " " flat "${log}")
    # CMake 4.4 prints "CMake Warning (deprecated) at", earlier versions "CMake Deprecation Warning at".
    set(deprecation "CMake (Deprecation Warning|Warning \\(deprecated\\)) at [^ ]+ \\(message\\): ENABLE_[A-Z_]+")
    set(pattern_MIGRATED "${deprecation} is deprecated and will be removed in WebFront 0\\.7\\.0: its value")
    set(pattern_KEPT "${deprecation} is deprecated [^:]+: use WEBFRONT_ENABLE_[A-Z_]+, which already has this value")
    set(pattern_IGNORED "CMake Warning at [^ ]+ \\(message\\): ENABLE_[A-Z_]+ is ignored: WEBFRONT_ENABLE_[A-Z_]+ is set")
    foreach(kind MIGRATED KEPT IGNORED)
        string(REGEX MATCHALL "${pattern_${kind}}" matches "${flat}")
        list(LENGTH matches count)
        if(NOT arg_${kind})
            set(arg_${kind} 0)
        endif()
        if(NOT count EQUAL arg_${kind})
            message(FATAL_ERROR "${name}: expected ${arg_${kind}} ${kind} messages, got ${count}\n${log}")
        endif()
    endforeach()
    string(REGEX MATCHALL "CMake (Deprecation )?Warning" all "${flat}")  # also "CMake Warning (deprecated)"
    list(LENGTH all total)
    math(EXPR expected_total "${arg_MIGRATED} + ${arg_KEPT} + ${arg_IGNORED}")
    if(NOT total EQUAL expected_total)
        message(FATAL_ERROR "${name}: expected ${expected_total} warnings in all, got ${total}\n${log}")
    endif()
endfunction()

configure("defaults" defaults
    EXPECT "TESTING=OFF LEGACY=<unset>" "FUZZING=OFF LEGACY=<unset>" "COVERAGE=OFF LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON)
configure("prefixed names at the top level" prefixed
    EXPECT "TESTING=ON LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DWEBFRONT_ENABLE_TESTING=ON -DWEBFRONT_ENABLE_FUZZING=ON -DWEBFRONT_ENABLE_COVERAGE=ON)

# A configured tree, then former names given on later command lines.
configure("former names at the top level are migrated" legacy MIGRATED 3
    EXPECT "TESTING=ON LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_TESTING=ON -DENABLE_FUZZING=ON -DENABLE_COVERAGE=ON)
configure("a migrated value is kept without warning" legacy
    EXPECT "TESTING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>")
configure("the prefixed name then decides" legacy
    EXPECT "TESTING=OFF LEGACY=<unset>" "FUZZING=ON LEGACY=<unset>"
    OPTIONS -DWEBFRONT_ENABLE_TESTING=OFF)
configure("a former name given later switches a cached prefixed value" legacy MIGRATED 2
    EXPECT "TESTING=ON LEGACY=<unset>" "FUZZING=OFF LEGACY=<unset>"
    OPTIONS -DENABLE_TESTING=ON -DENABLE_FUZZING=OFF)
configure("the same command again only reports the deprecation" legacy KEPT 2
    EXPECT "TESTING=ON LEGACY=<unset>" "FUZZING=OFF LEGACY=<unset>"
    OPTIONS -DENABLE_TESTING=ON -DENABLE_FUZZING=OFF)

# Both names on one command line: the prefixed one decides.
configure("the prefixed name wins when both are given" both IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_TESTING=ON -DWEBFRONT_ENABLE_TESTING=OFF)
configure("-Wno-deprecated does not hide an ignored value" both-quiet IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>"
    OPTIONS -Wno-deprecated -DTOP_LEVEL=ON -DENABLE_TESTING=ON -DWEBFRONT_ENABLE_TESTING=OFF)
configure("both names with one value only report the deprecation" both-same KEPT 1
    EXPECT "TESTING=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON -DENABLE_TESTING=ON -DWEBFRONT_ENABLE_TESTING=ON)

# An old tree holds typed entries: ENABLE_TESTING=ON and the FALSE/OFF defaults of the former sanitizer module.
configure("an old build tree" upgraded OPTIONS -DOLD_TREE=ON -DENABLE_TESTING=ON)
configure("an old tree's former defaults never override a new prefixed option" upgraded MIGRATED 1
    EXPECT "TESTING=ON LEGACY=<unset>" "COVERAGE=ON LEGACY=<unset>" "SANITIZER_ADDRESS=ON LEGACY=<unset>"
    OPTIONS -DOLD_TREE=OFF -DTOP_LEVEL=ON -DWEBFRONT_ENABLE_COVERAGE=ON -DWEBFRONT_ENABLE_SANITIZER_ADDRESS=ON)
configure("an old tree's OFF defaults" old-defaults OPTIONS -DOLD_TREE=ON)
configure("an old tree's OFF defaults are dropped silently" old-defaults
    EXPECT "TESTING=OFF LEGACY=<unset>" "COVERAGE=OFF LEGACY=<unset>" "SANITIZER_ADDRESS=OFF LEGACY=<unset>"
    OPTIONS -DOLD_TREE=OFF -DTOP_LEVEL=ON)

# A toolchain file sets normal variables, read after project() as the former option() calls did.
set(toolchain "-DCMAKE_TOOLCHAIN_FILE=${ROOT}/test/cmake/test-options/Toolchain.cmake")
configure("a toolchain file's former name is migrated" toolchain MIGRATED 1
    EXPECT "TESTING=ON LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON "${toolchain}" -DTOOLCHAIN_LEGACY=ON)
configure("the same toolchain file only reports the deprecation afterwards" toolchain KEPT 1
    EXPECT "TESTING=ON LEGACY=<unset>")
configure("a toolchain file's prefixed variable wins over its former name" toolchain-both IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>"
    OPTIONS -DTOP_LEVEL=ON "${toolchain}" -DTOOLCHAIN_LEGACY=ON -DTOOLCHAIN_PREFIXED=OFF)

# Normal variables still count as given when their values equal typed cache entries.
configure("cache before toolchain overrides" toolchain-cached
    EXPECT "TESTING=OFF LEGACY=<unset>" "CACHED_TESTING=OFF"
    OPTIONS -DTOP_LEVEL=ON "${toolchain}" -DWEBFRONT_ENABLE_TESTING=OFF)
configure("a matching prefixed normal variable preserves the cache" toolchain-cached IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>" "CACHED_TESTING=OFF"
    OPTIONS -DTOOLCHAIN_LEGACY=ON -DTOOLCHAIN_PREFIXED=OFF)
configure("repeating the matching override preserves the cache" toolchain-cached IGNORED 1
    EXPECT "TESTING=OFF LEGACY=<unset>" "CACHED_TESTING=OFF")
configure("removing the toolchain overrides keeps tests disabled" toolchain-cached
    EXPECT "TESTING=OFF LEGACY=<unset>" "CACHED_TESTING=OFF"
    OPTIONS -UTOOLCHAIN_LEGACY -UTOOLCHAIN_PREFIXED)

configure("cache before a matching former normal variable" toolchain-former-cached
    EXPECT "TESTING=ON LEGACY=<unset>" "CACHED_TESTING=ON"
    OPTIONS -DTOP_LEVEL=ON "${toolchain}" -DWEBFRONT_ENABLE_TESTING=ON)
configure("a matching former normal variable still migrates" toolchain-former-cached MIGRATED 1
    EXPECT "TESTING=OFF LEGACY=<unset>" "CACHED_TESTING=OFF"
    OPTIONS -DENABLE_TESTING:BOOL=OFF -DTOOLCHAIN_LEGACY=OFF)

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

# The announced removal cannot be forgotten: from 0.7.0 on, the migration refuses to configure.
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/test-options" -B "${WORK_DIR}/removal"
    "-DROOT=${ROOT}" -DTOP_LEVEL=ON -DFIXTURE_VERSION=0.7.0
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
string(REGEX REPLACE "[\n ]+" " " flat "${output}${error}")
if(status EQUAL 0 OR NOT flat MATCHES "must no longer read the former ENABLE_\\* test options")
    message(FATAL_ERROR "The migration of the former names must fail from WebFront 0.7.0 on: ${output}${error}")
endif()
message(STATUS "WebFront's test options ignore a parent's generic names and migrate the former ones")
