# Regression for the historical v0.1.1 release with a 0.1.0 header.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK)
    message(FATAL_ERROR "ROOT and WORK are required")
endif()
file(MAKE_DIRECTORY "${WORK}/archive/include/webfront")
file(WRITE "${WORK}/archive/include/webfront/WebFront.hpp"
    "inline constexpr std::string_view version = \"0.1.0\";\n")
file(MAKE_DIRECTORY "${WORK}/archive/include/webfront/system" "${WORK}/archive/docs")
file(WRITE "${WORK}/archive/include/webfront/DeveloperApp.hpp" "inline constexpr int apiVersion      = 1;\n")
file(WRITE "${WORK}/archive/include/webfront/system/DeveloperReleaseData.hpp"
    "inline constexpr std::string_view version{\"0.6.0\"};\n")
function(write_table rows)
    file(WRITE "${WORK}/archive/docs/DEVELOPER_ENVIRONMENT.md"
        "<!-- compatibility-table:begin -->\n| Library | Environment | Minimum api |\n| --- | --- | --- |\n| unreleased | 0.6.0 | 1 |\n${rows}<!-- compatibility-table:end -->\n")
endfunction()
write_table("| 0.1.0 | 0.6.0 | 1 |\n")

function(check_case description expected_result expected_message)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${WORK}/archive" ${ARGN}
        -P "${ROOT}/test/cmake/CheckReleaseVersion.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_result STREQUAL "success")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${description}: unexpected failure: ${output}${error}")
        endif()
    elseif(result EQUAL 0)
        message(FATAL_ERROR "${description}: mismatch was accepted")
    endif()
    if(NOT "${output}${error}" MATCHES "${expected_message}")
        message(FATAL_ERROR "${description}: missing diagnostic '${expected_message}': ${output}${error}")
    endif()
endfunction()

check_case("Archive without Git" success "version: 0.1.0" -DEXPECTED_VERSION=0.1.0)
check_case("Matching release" success "release tags checked: v0.1.0"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=v0.1.0)
check_case("Historical release mismatch" failure "Release tag 'v0.1.1' differs"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=v0.1.1)
check_case("CMake mismatch" failure "CMake version '0.1.1' differs"
    -DEXPECTED_VERSION=0.1.1)
check_case("Malformed release tag" failure "must have the form"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=release-0.1.0)
check_case("Empty release tag" failure "must have the form"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=)
write_table("")
check_case("Release without a compatibility row" failure "has no row"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=v0.1.0)
write_table("| 0.1.0 | 0.5.0 | 1 |\n")
check_case("Compatibility row that differs from the header" failure "names the environment"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=v0.1.0)
write_table("| 0.1.0 | 0.6.0 | 999 |\n")
check_case("Compatibility row with another minimum api" failure "minimum api"
    -DEXPECTED_VERSION=0.1.0 -DRELEASE_TAG=v0.1.0)
write_table("| 0.1.0 | 0.6.0 | 1 |\n")

find_package(Git REQUIRED QUIET)
# Use a fresh repository on every run; keep the archive fixture independent of it.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(repo "${WORK}/git-${suffix}")
file(MAKE_DIRECTORY "${repo}")
file(COPY "${WORK}/archive/include" DESTINATION "${repo}")
execute_process(COMMAND "${GIT_EXECUTABLE}" init "${repo}"
    COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" -c user.name=VersionTest -c user.email=version-test@example.invalid
    -c commit.gpgsign=false commit --allow-empty -m "Version fixture"
    WORKING_DIRECTORY "${repo}" COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" tag v0.1.1
    WORKING_DIRECTORY "${repo}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${repo}" -DEXPECTED_VERSION=0.1.0
    -P "${ROOT}/test/cmake/CheckReleaseVersion.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "Release tag 'v0.1.1' differs")
    message(FATAL_ERROR "Tag at HEAD mismatch was not detected: ${output}${error}")
endif()
# Only this test's freshly created repository is removed.
file(REMOVE_RECURSE "${repo}")
