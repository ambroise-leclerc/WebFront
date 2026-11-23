# SPDX-License-Identifier: Apache-2.0 OR CECILL-B
# Registers the scenarios of a webfront::bdd test executable in CTest, one case per scenario, named "Scenario: <title>".
# This is the public counterpart of Catch2's catch_discover_tests (docs/BDD.md); its name and options follow WebFront's
# versions (docs/CHANGELOG.md). Obtain it with the WebFront::Bdd target:
#
#   find_package(WebFrontBdd REQUIRED)          # an installed WebFront
#   # or add_subdirectory()/CPM/FetchContent of WebFront, which defines WebFront::Bdd and includes this file
#   target_link_libraries(my_tests PRIVATE WebFront::Bdd)
#   webfront_bdd_discover_tests(my_tests [TEST_PREFIX <prefix>] [PROPERTIES <name> <value>...]
#                               [SHARDS <n> [SHARD_NAME <name>] [JUNIT_PREFIX <path>]])
#
#   TEST_PREFIX   text put before "Scenario: " in each case name.
#   PROPERTIES    properties of every case (TIMEOUT, LABELS, ENVIRONMENT...); a skipped scenario exits with 4, which the
#                 cases treat as SKIP_RETURN_CODE.
#   SHARDS        registers <n> cases instead, each running a share of the scenarios in declaration order in one process
#                 (`--order decl --shard-count <n> --shard-index <i>`), which saves the start of one process per scenario.
#                 They are named <SHARD_NAME>-<i> (default "<target>Shards") and write a JUnit report to
#                 <JUNIT_PREFIX>-<i>.xml (default "${CMAKE_CURRENT_BINARY_DIR}/<target>-results").
#
# After each build of the target, the executable lists its scenarios (`--list`) and this file, run as a script, writes the
# add_test() calls that CTest includes. Each case runs `<target> --exact <title>`.

if(CMAKE_SCRIPT_MODE_FILE)
    # Keep properties in a generated file: passing shell metacharacters through a Windows post-build command is unreliable.
    execute_process(COMMAND "${EXECUTABLE}" --list OUTPUT_VARIABLE titles ERROR_VARIABLE errors RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${EXECUTABLE} --list failed (${result}): ${errors}")
    endif()
    include("${PROPERTIES_FILE}")
    set(quoted_properties)
    foreach(property IN LISTS properties)
        string(APPEND quoted_properties " [==[${property}]==]")
    endforeach()
    # A title may hold a ';', which splits a CMake list, or an unpaired '[' or ']', inside which a list is not split.
    string(REPLACE ";" "<bdd-semicolon>" titles "${titles}")
    string(REPLACE "[" "<bdd-open>" titles "${titles}")
    string(REPLACE "]" "<bdd-close>" titles "${titles}")
    string(REPLACE "\n" ";" titles "${titles}")
    set(content "")
    foreach(title IN LISTS titles)
        if(title STREQUAL "")
            continue()
        endif()
        string(REPLACE "<bdd-semicolon>" ";" title "${title}")
        string(REPLACE "<bdd-open>" "[" title "${title}")
        string(REPLACE "<bdd-close>" "]" title "${title}")
        set(name "${PREFIX}Scenario: ${title}")
        string(APPEND content "add_test([==[${name}]==] [==[${EXECUTABLE}]==] --exact [==[${title}]==])\n"
            "set_tests_properties([==[${name}]==] PROPERTIES SKIP_RETURN_CODE 4${quoted_properties})\n")
    endforeach()
    file(WRITE "${OUTPUT}" "${content}")
    return()
endif()

set(_WEBFRONT_BDD_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(webfront_bdd_discover_tests target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEST_PREFIX;SHARDS;SHARD_NAME;JUNIT_PREFIX" "PROPERTIES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "webfront_bdd_discover_tests: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "webfront_bdd_discover_tests: ${target} is not a target")
    endif()
    if(DEFINED arg_SHARDS)
        if(NOT arg_SHARDS MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "webfront_bdd_discover_tests: SHARDS must be a positive integer, not '${arg_SHARDS}'")
        endif()
        if(NOT arg_SHARD_NAME)
            set(arg_SHARD_NAME "${target}Shards")
        endif()
        if(NOT arg_JUNIT_PREFIX)
            set(arg_JUNIT_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/${target}-results")
        endif()
        math(EXPR last_shard "${arg_SHARDS} - 1")
        foreach(shard RANGE ${last_shard})
            add_test(NAME ${arg_SHARD_NAME}-${shard}
                COMMAND ${target} --order decl --shard-count ${arg_SHARDS} --shard-index ${shard}
                        --junit "${arg_JUNIT_PREFIX}-${shard}.xml")
            # A shard whose every scenario is skipped exits with 4, as a single scenario does.
            set_tests_properties(${arg_SHARD_NAME}-${shard} PROPERTIES SKIP_RETURN_CODE 4 ${arg_PROPERTIES})
        endforeach()
        return()
    endif()
    set(base "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(properties_file "${base}_properties-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${properties_file}" CONTENT "set(properties [==[${arg_PROPERTIES}]==])\n")
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(output "${base}_tests-$<CONFIG>.cmake")
        string(CONCAT include_content "if(EXISTS \"${base}_tests-\${CTEST_CONFIGURATION_TYPE}.cmake\")\n"
                            "  include(\"${base}_tests-\${CTEST_CONFIGURATION_TYPE}.cmake\")\n"
                            "else()\n"
                            "  add_test([==[${target}_NOT_BUILT]==] [==[${target}_NOT_BUILT]==])\n"
                            "endif()\n")
    else()
        set(output "${base}_tests.cmake")
        string(CONCAT include_content "if(EXISTS \"${output}\")\n  include(\"${output}\")\n"
                            "else()\n  add_test([==[${target}_NOT_BUILT]==] [==[${target}_NOT_BUILT]==])\nendif()\n")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:${target}>" "-DOUTPUT=${output}" "-DPREFIX=${arg_TEST_PREFIX}"
                "-DPROPERTIES_FILE=${properties_file}" -P "${_WEBFRONT_BDD_SCRIPT}"
        VERBATIM)
    file(WRITE "${base}_include.cmake" "${include_content}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${base}_include.cmake")
endfunction()
