# Offline release check. EXPECTED_VERSION comes from project(WebFront VERSION ...).
# CI passes RELEASE_TAG for tag pushes and published releases; local CTest also
# checks version tags pointing at HEAD. Source archives need neither Git nor network.
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT DEFINED EXPECTED_VERSION)
    message(FATAL_ERROR "ROOT and EXPECTED_VERSION are required")
endif()

file(STRINGS "${ROOT}/include/webfront/WebFront.hpp" declarations
    REGEX "^inline constexpr std::string_view version = \"[0-9]+\\.[0-9]+\\.[0-9]+\";")
list(LENGTH declarations count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "Expected one version declaration in WebFront.hpp")
endif()
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" header_version "${declarations}")
if(NOT "${EXPECTED_VERSION}" STREQUAL "${header_version}")
    message(FATAL_ERROR "CMake version '${EXPECTED_VERSION}' differs from WebFront.hpp '${header_version}'")
endif()

set(release_tags "")
if(DEFINED RELEASE_TAG)
    if(NOT RELEASE_TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Release tag '${RELEASE_TAG}' must have the form vMAJOR.MINOR.PATCH")
    endif()
    list(APPEND release_tags "${RELEASE_TAG}")
endif()

# A worktree has a .git file, a checkout a .git directory, an archive neither.
if(EXISTS "${ROOT}/.git")
    find_package(Git REQUIRED QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
        WORKING_DIRECTORY "${ROOT}" RESULT_VARIABLE result OUTPUT_VARIABLE tags
        ERROR_VARIABLE error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Cannot inspect release tags: ${error}")
    endif()
    string(REPLACE "\n" ";" tags "${tags}")
    foreach(tag IN LISTS tags)
        if(tag MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
            list(APPEND release_tags "${tag}")
        endif()
    endforeach()
endif()

foreach(tag IN LISTS release_tags)
    if(NOT "${tag}" STREQUAL "v${header_version}")
        message(FATAL_ERROR "Release tag '${tag}' differs from WebFront.hpp/CMake 'v${header_version}'")
    endif()
endforeach()
message(STATUS "WebFront.hpp/CMake version: ${header_version}; release tags checked: ${release_tags}")

# ADR-021: a released library version has a row in the compatibility table of docs/DEVELOPER_ENVIRONMENT.md, and that row
# names the environment that the header of the release commit pins.
if(release_tags)
    file(READ "${ROOT}/include/webfront/system/DeveloperReleaseData.hpp" pin_header)
    if(NOT pin_header MATCHES "version\{\"([^\"]*)\"\}")
        message(FATAL_ERROR "DeveloperReleaseData.hpp has no version")
    endif()
    set(pinned_environment "${CMAKE_MATCH_1}")
    file(READ "${ROOT}/docs/DEVELOPER_ENVIRONMENT.md" environment_document)
    string(REPLACE "." "[.]" version_pattern "${header_version}")
    if(NOT environment_document MATCHES "\n[|] ${version_pattern} [|] ([^ |]+) [|] ([0-9]+) [|]\n")
        message(FATAL_ERROR "The compatibility table of docs/DEVELOPER_ENVIRONMENT.md has no row for the library ${header_version}; "
            "freeze it with cmake/DeveloperCompatibility.cmake (MODE=freeze)")
    endif()
    set(row_environment "${CMAKE_MATCH_1}")
    set(row_api "${CMAKE_MATCH_2}")
    if(NOT "${row_environment}" STREQUAL "${pinned_environment}")
        message(FATAL_ERROR "The compatibility row of ${header_version} names the environment ${row_environment}, "
            "but DeveloperReleaseData.hpp pins ${pinned_environment}")
    endif()
    # The minimum api is developer::apiVersion of the release commit.
    file(READ "${ROOT}/include/webfront/DeveloperApp.hpp" application_header)
    if(NOT application_header MATCHES "apiVersion *= *([0-9]+);")
        message(FATAL_ERROR "DeveloperApp.hpp has no apiVersion")
    endif()
    if(NOT "${row_api}" STREQUAL "${CMAKE_MATCH_1}")
        message(FATAL_ERROR "The compatibility row of ${header_version} gives the minimum api ${row_api}, "
            "but developer::apiVersion is ${CMAKE_MATCH_1}")
    endif()
    message(STATUS "Compatibility table: ${header_version} -> environment ${pinned_environment}, api ${row_api}")
endif()
