# Registers the scenarios of PROBE with WebFrontBdd.cmake (SCRIPT) and checks that CTest sees one case per scenario,
# whatever brackets and semicolons their titles hold.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/properties.cmake" "set(properties [==[TIMEOUT;10;LABELS;one|two]==])\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=${PROBE}" "-DOUTPUT=${WORK}/tests.cmake" -DPREFIX=Probe.
                        "-DPROPERTIES_FILE=${WORK}/properties.cmake" -P "${SCRIPT}" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The discovery script failed: ${errors}")
endif()
file(READ "${WORK}/tests.cmake" generated)
if(NOT generated MATCHES "\\[==\\[one\\|two\\]==\\]")
    message(FATAL_ERROR "Discovery did not preserve a pipe in the test properties")
endif()
file(WRITE "${WORK}/CTestTestfile.cmake" "include(\"${WORK}/tests.cmake\")\n")
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" -N WORKING_DIRECTORY "${WORK}" OUTPUT_VARIABLE listed RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ctest -N failed:\n${listed}")
endif()
foreach(title IN ITEMS "A [b title" "Second\; one" "Third ]x" "Fourth [tag] title" "Literal <bdd-open> %5B %25 <bdd-semicolon>" "Ends like a delimiter ]==" "Ends with a backslash \\")
    string(REPLACE "\;" ";" title "${title}")
    string(FIND "${listed}" ": Probe.Scenario: ${title}\n" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "CTest does not list \"Probe.Scenario: ${title}\":\n${listed}")
    endif()
endforeach()
if(NOT listed MATCHES "Total Tests: 7\n")
    message(FATAL_ERROR "CTest should list 7 cases:\n${listed}")
endif()

# Exercise the actual post-build command through cmd.exe: pipes in its arguments used to break Ninja on Windows.
if(WIN32 AND GENERATOR STREQUAL "Ninja")
    file(MAKE_DIRECTORY "${WORK}/project")
    file(WRITE "${WORK}/project/main.cpp" "#include <iostream>\nint main() { std::cout << \"probe\\n\"; }\n")
    file(WRITE "${WORK}/project/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.31)\nproject(DiscoveryProbe LANGUAGES CXX)\ninclude([==[${SCRIPT}]==])\nadd_executable(probe main.cpp)\nwebfront_bdd_discover_tests(probe TEST_PREFIX [=[a;b.]=] PROPERTIES FIXTURES_CLEANUP SHARDS TIMEOUT 10 ENVIRONMENT [=[A=b\\]=] FIXTURES_SETUP [=[fx]==]=] FIXTURES_REQUIRED [=[x;y]=] LABELS [==[one|two]==])\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/build" -G Ninja
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_CROSSCOMPILING_EMULATOR=${CMAKE_COMMAND};-E;env"
        RESULT_VARIABLE configured OUTPUT_VARIABLE configure_output ERROR_VARIABLE configure_error)
    if(NOT configured EQUAL 0)
        message(FATAL_ERROR "Discovery regression configuration failed: ${configure_output}${configure_error}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build"
        RESULT_VARIABLE built OUTPUT_VARIABLE build_output ERROR_VARIABLE build_error)
    if(NOT built EQUAL 0)
        message(FATAL_ERROR "Discovery post-build command failed: ${build_output}${build_error}")
    endif()
    file(READ "${WORK}/build/probe_tests.cmake" actual)
    string(FIND "${actual}" "[==[-E]==] [==[env]==]" emulator_used)
    if(emulator_used EQUAL -1)
        message(FATAL_ERROR "The target emulator is not used by the cases:\n${actual}")
    endif()
    # A value ending in a backslash or like a closing delimiter neither merges with its neighbour nor closes its argument.
    string(FIND "${actual}" "[==[A=b\\]==]" backslash_kept)
    string(FIND "${actual}" "[===[fx]==]===]" delimiter_kept)
    string(FIND "${actual}" "[==[x;y]==]" semicolon_kept)
    string(FIND "${actual}" "[==[a;b.Scenario: probe]==]" prefix_kept)
    string(FIND "${actual}" "[==[FIXTURES_CLEANUP]==] [==[SHARDS]==]" keyword_kept)
    if(backslash_kept EQUAL -1 OR delimiter_kept EQUAL -1 OR semicolon_kept EQUAL -1 OR prefix_kept EQUAL -1 OR keyword_kept EQUAL -1)
        message(FATAL_ERROR "The cases do not keep the properties as written:\n${actual}")
    endif()
    if(NOT actual MATCHES "\\[==\\[one\\|two\\]==\\]")
        message(FATAL_ERROR "The post-build command did not preserve the property")
    endif()
endif()

# A second call without SHARDS on a target is refused; property values and prefixes that look like a closing delimiter are not (#648).
file(MAKE_DIRECTORY "${WORK}/refusals")
file(WRITE "${WORK}/refusals/main.cpp" "int main() {}\n")
function(configure_probe name calls)
    file(WRITE "${WORK}/refusals/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.31)\nproject(Refusal LANGUAGES CXX)\ninclude([==[${SCRIPT}]==])\nenable_testing()\nadd_executable(probe main.cpp)\n${calls}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/refusals" -B "${WORK}/refusals/build" -G "${GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(status "${status}" PARENT_SCOPE)
    set(log "${out}${err}" PARENT_SCOPE)
endfunction()

configure_probe(twice "webfront_bdd_discover_tests(probe)\nwebfront_bdd_discover_tests(probe)")
if(status EQUAL 0 OR NOT log MATCHES "already registered")
    message(FATAL_ERROR "A second call on a target was not refused (${status}):\n${log}")
endif()
configure_probe(bracket "webfront_bdd_discover_tests(probe PROPERTIES ENVIRONMENT [=[A=]==]]=])")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "A property holding ]==] must be accepted:
${log}")
endif()
configure_probe(both "webfront_bdd_discover_tests(probe)\nwebfront_bdd_discover_tests(probe SHARDS 2)")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "A call with SHARDS must accompany a call without:\n${log}")
endif()

# A change of the properties alone does not rebuild the executable, and must still reach the registered cases (#648).
if(WIN32 AND GENERATOR STREQUAL "Ninja")
    file(READ "${WORK}/project/CMakeLists.txt" project_text)
    string(REPLACE "TIMEOUT 10" "TIMEOUT 20" project_text "${project_text}")
    file(WRITE "${WORK}/project/CMakeLists.txt" "${project_text}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/project" -B "${WORK}/build" RESULT_VARIABLE reconfigured ERROR_VARIABLE reconfigure_error)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build" --target probe RESULT_VARIABLE rebuilt ERROR_VARIABLE rebuild_error)
    if(NOT reconfigured EQUAL 0 OR NOT rebuilt EQUAL 0)
        message(FATAL_ERROR "Reconfiguring with other properties failed: ${reconfigure_error}${rebuild_error}")
    endif()
    file(READ "${WORK}/build/probe_tests.cmake" regenerated)
    if(NOT regenerated MATCHES "\\[==\\[TIMEOUT\\]==\\] \\[==\\[20\\]==\\]")
        message(FATAL_ERROR "The cases keep the old properties after a reconfiguration:\n${regenerated}")
    endif()
endif()

# A target with a CROSSCOMPILING_EMULATOR lists its scenarios and runs its cases through it (#648).
execute_process(COMMAND "${CMAKE_COMMAND}" "-DEXECUTABLE=${PROBE}" "-DOUTPUT=${WORK}/emulated.cmake" -DPREFIX=Probe.
                        "-DEMULATOR=${CMAKE_COMMAND};-E;env" "-DPROPERTIES_FILE=${WORK}/properties.cmake" -P "${SCRIPT}"
                RESULT_VARIABLE emulated ERROR_VARIABLE emulated_error)
if(NOT emulated EQUAL 0)
    message(FATAL_ERROR "The discovery through an emulator failed: ${emulated_error}")
endif()
file(READ "${WORK}/emulated.cmake" emulated_cases)
string(FIND "${emulated_cases}" "-E" emulator_position)
string(FIND "${emulated_cases}" "env" env_position)
string(FIND "${emulated_cases}" "--exact" exact_position)
if(emulator_position EQUAL -1 OR env_position EQUAL -1 OR NOT env_position LESS exact_position)
    message(FATAL_ERROR "The cases do not run through the emulator:\n${emulated_cases}")
endif()

configure_probe(prefix "webfront_bdd_discover_tests(probe TEST_PREFIX [=[a]==]b]=])")
if(NOT status EQUAL 0)
    message(FATAL_ERROR "A prefix holding ]==] must be accepted:
${log}")
endif()

# An executable excluded from the default build stays excluded: discovery does not bring it back (#648).
file(WRITE "${WORK}/refusals/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.31)\nproject(Excluded LANGUAGES CXX)\ninclude([==[${SCRIPT}]==])\nenable_testing()\nadd_executable(probe EXCLUDE_FROM_ALL main.cpp)\nwebfront_bdd_discover_tests(probe)\n")
file(REMOVE_RECURSE "${WORK}/refusals/excluded")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${WORK}/refusals" -B "${WORK}/refusals/excluded" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" RESULT_VARIABLE configured_excluded ERROR_VARIABLE excluded_error OUTPUT_VARIABLE excluded_output)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/refusals/excluded" RESULT_VARIABLE built_excluded ERROR_VARIABLE excluded_error2 OUTPUT_VARIABLE excluded_output2)
file(GLOB_RECURSE built_probe "${WORK}/refusals/excluded/probe" "${WORK}/refusals/excluded/probe.exe")
if(NOT configured_excluded EQUAL 0 OR NOT built_excluded EQUAL 0 OR built_probe)
    message(FATAL_ERROR "A complete build compiled an executable excluded from it (${configured_excluded}, ${built_excluded}): ${built_probe}\n${excluded_error}${excluded_error2}")
endif()

# A path that holds ]==] reaches the discovery script intact, as the executable and as the file that names it.
set(odd_dir "${WORK}/odd]==]dir")
file(MAKE_DIRECTORY "${odd_dir}")
get_filename_component(probe_name "${PROBE}" NAME)
file(COPY "${PROBE}" DESTINATION "${odd_dir}")
file(WRITE "${odd_dir}/executable.txt" "${odd_dir}/${probe_name}")
file(WRITE "${odd_dir}/emulator.txt" "")
file(WRITE "${odd_dir}/odd-properties.cmake"
    "set(properties [==[]==])\nset(bdd_executable_file [===[${odd_dir}/executable.txt]===])\nset(bdd_emulator_file [===[${odd_dir}/emulator.txt]===])\nset(bdd_output [===[${odd_dir}/odd-tests.cmake]===])\nset(bdd_prefix [==[]==])\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROPERTIES_FILE=${odd_dir}/odd-properties.cmake" -P "${SCRIPT}"
    RESULT_VARIABLE odd_result ERROR_VARIABLE odd_error)
if(NOT odd_result EQUAL 0)
    message(FATAL_ERROR "The discovery failed for an executable in a directory holding ]==]: ${odd_error}")
endif()
file(READ "${odd_dir}/odd-tests.cmake" odd_cases)
if(NOT odd_cases MATCHES "odd\]==\]dir")
    message(FATAL_ERROR "The executable path was altered:\n${odd_cases}")
endif()

# An executable under a directory whose name holds ';' stays one argument of its case (#648).
set(semicolon_dir "${WORK}/build;debug")
file(MAKE_DIRECTORY "${semicolon_dir}")
file(COPY "${PROBE}" DESTINATION "${semicolon_dir}")
file(WRITE "${semicolon_dir}/executable.txt" "${semicolon_dir}/${probe_name}")
file(WRITE "${semicolon_dir}/emulator.txt" "")
file(WRITE "${semicolon_dir}/semicolon-properties.cmake"
    "set(properties [==[]==])\nset(bdd_executable_file [===[${semicolon_dir}/executable.txt]===])\nset(bdd_emulator_file [===[${semicolon_dir}/emulator.txt]===])\nset(bdd_output [===[${semicolon_dir}/semicolon-tests.cmake]===])\nset(bdd_prefix [==[]==])\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROPERTIES_FILE=${semicolon_dir}/semicolon-properties.cmake" -P "${SCRIPT}"
    RESULT_VARIABLE semicolon_result ERROR_VARIABLE semicolon_error)
if(NOT semicolon_result EQUAL 0)
    message(FATAL_ERROR "The discovery failed for an executable under a directory holding ';': ${semicolon_error}")
endif()
file(READ "${semicolon_dir}/semicolon-tests.cmake" semicolon_cases)
string(FIND "${semicolon_cases}" "[==[${semicolon_dir}/${probe_name}]==] --exact" whole_path)
if(whole_path EQUAL -1)
    message(FATAL_ERROR "The executable path was split:\n${semicolon_cases}")
endif()
