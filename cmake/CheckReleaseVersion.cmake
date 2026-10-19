# Offline release check. EXPECTED_VERSION comes from project(WebFront VERSION ...).
# CI passes RELEASE_TAG for tag pushes and published releases; local CTest also
# checks version tags pointing at HEAD. Source archives need neither Git nor network.
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT DEFINED EXPECTED_VERSION)
    message(FATAL_ERROR "ROOT and EXPECTED_VERSION are required")
endif()

file(STRINGS "${ROOT}/include/WebFront.hpp" declarations
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
