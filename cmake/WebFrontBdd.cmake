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
#   TEST_PREFIX   text put before "Scenario: " in each case name; refused with SHARDS (use SHARD_NAME).
#   PROPERTIES    properties of every case (TIMEOUT, LABELS, ENVIRONMENT...). A skipped scenario exits with 4, which the
#                 cases treat as SKIP_RETURN_CODE; a SKIP_RETURN_CODE of yours replaces it.
#   SHARDS        registers <n> cases instead, each running a share of the scenarios in declaration order in one process
#                 (`--order decl --shard-count <n> --shard-index <i>`), which saves the start of one process per scenario.
#                 They are named <SHARD_NAME>-<i> (default "<target>Shards") and write a JUnit report to
#                 <JUNIT_PREFIX>-<i>.xml (default "${CMAKE_CURRENT_BINARY_DIR}/<target>-results").
#
# After each build of the target, the executable lists its scenarios (`--list`) and this file, run as a script, writes the
# add_test() calls that CTest includes. Each case runs `<target> --exact <title>`, through the CROSSCOMPILING_EMULATOR of the
# target when it has one (which also lists the scenarios at build time).

# A bracket argument whose delimiter occurs neither in the value nor at the junction with its own closing bracket.
function(bdd_quote out value)
    set(equals "==")
    string(LENGTH "${value}" length)
    while(TRUE)
        string(FIND "${value}]${equals}]" "]${equals}]" position)
        if(position EQUAL length)
            break()
        endif()
        string(APPEND equals "=")
    endwhile()
    set(${out} "[${equals}[${value}]${equals}]" PARENT_SCOPE)
endfunction()

# Values travel in CMake lists, which a ';', a backslash before the next ';', or an unpaired bracket would split or join
# wrongly: they are escaped as percent codes, "%" first so that a value which holds a code of its own comes back unchanged.
function(bdd_escape out value)
    string(REPLACE "%" "%25" value "${value}")
    string(REPLACE ";" "%3B" value "${value}")
    string(REPLACE "\\" "%5C" value "${value}")
    string(REPLACE "[" "%5B" value "${value}")
    string(REPLACE "]" "%5D" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
function(bdd_unescape out value)
    string(REPLACE "%3B" ";" value "${value}")
    string(REPLACE "%5C" "\\" value "${value}")
    string(REPLACE "%5B" "[" value "${value}")
    string(REPLACE "%5D" "]" value "${value}")
    string(REPLACE "%25" "%" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    # With CMAKE_CROSSCOMPILING_EMULATOR the executable cannot run alone: the emulator lists the scenarios and runs each case.
    execute_process(COMMAND ${EMULATOR} "${EXECUTABLE}" --list OUTPUT_VARIABLE titles ERROR_VARIABLE errors RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${EMULATOR} ${EXECUTABLE} --list failed (${result}): ${errors}")
    endif()
    set(command_prefix)
    foreach(word IN LISTS EMULATOR EXECUTABLE)
        bdd_quote(quoted "${word}")
        string(APPEND command_prefix " ${quoted}")
    endforeach()
    # Keep properties in a generated file: passing shell metacharacters through a Windows post-build command is unreliable.
    include("${PROPERTIES_FILE}")
    set(quoted_properties)
    foreach(property IN LISTS properties)
        bdd_unescape(property "${property}")
        bdd_quote(quoted "${property}")
        string(APPEND quoted_properties " ${quoted}")
    endforeach()
    bdd_escape(titles "${titles}")
    string(REPLACE "\n" ";" titles "${titles}")
    set(content "")
    foreach(title IN LISTS titles)
        if(title STREQUAL "")
            continue()
        endif()
        bdd_unescape(title "${title}")
        bdd_quote(quoted_name "${PREFIX}Scenario: ${title}")
        bdd_quote(quoted_title "${title}")
        string(APPEND content "add_test(${quoted_name}${command_prefix} --exact ${quoted_title})\n"
            "set_tests_properties(${quoted_name} PROPERTIES SKIP_RETURN_CODE 4${quoted_properties})\n")
    endforeach()
    file(WRITE "${OUTPUT}" "${content}")
    return()
endif()

function(webfront_bdd_discover_tests target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEST_PREFIX;SHARDS;SHARD_NAME;JUNIT_PREFIX" "PROPERTIES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "webfront_bdd_discover_tests: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    # PROPERTIES is read from the arguments themselves: cmake_parse_arguments keeps its values in a list, where a value that
    # ends with a backslash would merge with the next one. Each value is kept escaped (bdd_escape) from here on.
    set(escaped_properties)
    set(collecting FALSE)
    if(ARGC GREATER 1)
        math(EXPR last_argument "${ARGC} - 1")
        foreach(index RANGE 1 ${last_argument})
            set(word "${ARGV${index}}")
            if(word STREQUAL "PROPERTIES")
                set(collecting TRUE)
            elseif(word MATCHES "^(TEST_PREFIX|SHARDS|SHARD_NAME|JUNIT_PREFIX)$")
                set(collecting FALSE)
            elseif(collecting)
                bdd_escape(word "${word}")
                list(APPEND escaped_properties "${word}")
            endif()
        endforeach()
    endif()
    list(LENGTH escaped_properties property_count)
    math(EXPR odd_properties "${property_count} % 2")
    if(odd_properties)
        message(FATAL_ERROR "webfront_bdd_discover_tests: PROPERTIES takes pairs of a name and a value")
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "webfront_bdd_discover_tests: ${target} is not a target")
    endif()
    if(NOT DEFINED arg_SHARDS AND (arg_SHARD_NAME OR arg_JUNIT_PREFIX))
        message(FATAL_ERROR "webfront_bdd_discover_tests: SHARD_NAME and JUNIT_PREFIX apply only with SHARDS")
    endif()
    if(DEFINED arg_SHARDS)
        if(arg_TEST_PREFIX)
            message(FATAL_ERROR "webfront_bdd_discover_tests: TEST_PREFIX does not apply to SHARDS; name the shards with SHARD_NAME")
        endif()
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
            # A shard whose every scenario is skipped exits with 4, as a single scenario does; a SKIP_RETURN_CODE of
            # PROPERTIES, listed after this one, takes precedence.
            set_tests_properties(${arg_SHARD_NAME}-${shard} PROPERTIES SKIP_RETURN_CODE 4)
            if(property_count GREATER 0)
                math(EXPR last_property "${property_count} - 2")
                foreach(position RANGE 0 ${last_property} 2)
                    math(EXPR value_position "${position} + 1")
                    list(GET escaped_properties ${position} property_name)
                    list(GET escaped_properties ${value_position} property_value)
                    bdd_unescape(property_name "${property_name}")
                    bdd_unescape(property_value "${property_value}")
                    set_tests_properties(${arg_SHARD_NAME}-${shard} PROPERTIES "${property_name}" "${property_value}")
                endforeach()
            endif()
        endforeach()
        return()
    endif()
    get_property(already TARGET ${target} PROPERTY WEBFRONT_BDD_DISCOVERED)
    if(already)
        message(FATAL_ERROR "webfront_bdd_discover_tests: ${target} is already registered; a second call would overwrite the first one's cases (SHARDS calls can accompany it)")
    endif()
    set_property(TARGET ${target} PROPERTY WEBFRONT_BDD_DISCOVERED TRUE)
    set(base "${CMAKE_CURRENT_BINARY_DIR}/${target}")
    set(properties_file "${base}_properties-$<CONFIG>.cmake")
    # The properties go into the file escaped (no bracket, backslash or ';' of theirs can change how CMake reads it) and the
    # script that discovers the cases unescapes them. The prefix is there as well: a change of either makes the file newer
    # than the cases generated from it.
    bdd_escape(escaped_prefix "${arg_TEST_PREFIX}")
    file(GENERATE OUTPUT "${properties_file}" CONTENT "set(properties [==[${escaped_properties}]==])\n# prefix: ${escaped_prefix}\n")
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
    set(discovery_command "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:${target}>" "-DOUTPUT=${output}" "-DPREFIX=${arg_TEST_PREFIX}" "-DEMULATOR=$<TARGET_PROPERTY:${target},CROSSCOMPILING_EMULATOR>"
                "-DPROPERTIES_FILE=${properties_file}" -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    # Right after each build of the target, so that building it alone registers its cases...
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${discovery_command} VERBATIM)
    # ...and again when only the properties or the prefix changed: that reconfiguration does not rebuild the executable.
    add_custom_command(OUTPUT "${output}" COMMAND ${discovery_command} DEPENDS ${target} "${properties_file}" VERBATIM)
    add_custom_target(${target}_bdd_discovery ALL DEPENDS "${output}")
    file(WRITE "${base}_include.cmake" "${include_content}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${base}_include.cmake")
endfunction()
