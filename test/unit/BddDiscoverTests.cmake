# Registers one CTest case per scenario of a Bdd.hpp test executable, named "Scenario: <title>".
#
#   bdd_discover_tests(<target> [TEST_PREFIX <prefix>] [PROPERTIES <name> <value>...])
#
# After each build of the target, the executable lists its scenarios (`--list`) and this file, run as a script, writes the
# add_test() calls that CTest includes. Each case runs `<target> --exact <title>`; a skipped scenario exits with 4.

if(CMAKE_SCRIPT_MODE_FILE)
    # Script mode: EXECUTABLE, OUTPUT, PREFIX and PROPERTIES (its ';' encoded as '|') come from -D.
    execute_process(COMMAND "${EXECUTABLE}" --list OUTPUT_VARIABLE titles ERROR_VARIABLE errors RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${EXECUTABLE} --list failed (${result}): ${errors}")
    endif()
    string(REPLACE "|" ";" properties "${PROPERTIES}")
    set(quoted_properties)
    foreach(property IN LISTS properties)
        string(APPEND quoted_properties " [==[${property}]==]")
    endforeach()
    # A title may hold a ';', which CMake lists would split.
    string(REPLACE ";" "<semicolon>" titles "${titles}")
    string(REPLACE "\n" ";" titles "${titles}")
    set(content "")
    foreach(title IN LISTS titles)
        if(title STREQUAL "")
            continue()
        endif()
        string(REPLACE "<semicolon>" ";" title "${title}")
        set(name "${PREFIX}Scenario: ${title}")
        string(APPEND content "add_test([==[${name}]==] [==[${EXECUTABLE}]==] --exact [==[${title}]==])\n"
            "set_tests_properties([==[${name}]==] PROPERTIES SKIP_RETURN_CODE 4${quoted_properties})\n")
    endforeach()
    file(WRITE "${OUTPUT}" "${content}")
    return()
endif()

set(_BDD_DISCOVER_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(bdd_discover_tests target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEST_PREFIX" "PROPERTIES")
    string(REPLACE ";" "|" properties "${arg_PROPERTIES}")
    set(base "${CMAKE_CURRENT_BINARY_DIR}/${target}")
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
                "-DPROPERTIES=${properties}" -P "${_BDD_DISCOVER_SCRIPT}"
        VERBATIM)
    file(WRITE "${base}_include.cmake" "${include_content}")
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${base}_include.cmake")
endfunction()
