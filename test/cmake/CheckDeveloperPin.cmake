# Offline consistency of the developer pin (ADR-021, decision 2), in pure CMake. The three places that name a version:
#   distribution.json         the sources' version (may be ahead of the pin: state (a))
#   DeveloperReleaseData.hpp  the version applications accept
#   WebFrontBootstrap.cmake   the version the tests run (the header's, or an earlier published one: state (c))
# Normal: distribution == header or ahead of it; bootstrap <= header. Inconsistent: a header newer than distribution.json, a
# header whose URL is not the one of its own version, a bootstrap newer than the header, or a bootstrap equal to the header
# with another URL or integrity. The bytes and the origin of the pinned URL are checked by `wf run pin`; a header equal to a
# fresh build, by the comparison of the Developer packages workflow. ROOT is the repository (or a fixture of it).
cmake_minimum_required(VERSION 3.31)
include("${CMAKE_CURRENT_LIST_DIR}/SemVer.cmake")
if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

file(READ "${ROOT}/js/developer/tools/distribution.json" distribution)
string(JSON sources_version GET "${distribution}" version)

file(READ "${ROOT}/include/webfront/system/DeveloperReleaseData.hpp" header)
foreach(field version environment integrity)
    if(NOT header MATCHES "${field}\{\"([^\"]*)\"\}")
        message(FATAL_ERROR "DeveloperReleaseData.hpp has no ${field}")
    endif()
    set(header_${field} "${CMAKE_MATCH_1}")
endforeach()

file(READ "${ROOT}/cmake/WebFrontBootstrap.cmake" bootstrap)
foreach(field VERSION URL INTEGRITY)
    if(NOT bootstrap MATCHES "WEBFRONT_BOOTSTRAP_${field} \"([^\"]*)\"")
        message(FATAL_ERROR "WebFrontBootstrap.cmake has no WEBFRONT_BOOTSTRAP_${field}")
    endif()
    set(bootstrap_${field} "${CMAKE_MATCH_1}")
endforeach()

# SemVer precedence: CMake's VERSION_* comparisons ignore the prerelease (0.7.0-rc.2 against 0.7.0-rc.1).
webfront_semver_compare("${header_version}" "${sources_version}" header_to_sources)
webfront_semver_compare("${bootstrap_VERSION}" "${header_version}" bootstrap_to_header)
if(header_to_sources GREATER 0)
    message(FATAL_ERROR "DeveloperReleaseData.hpp pins ${header_version}, newer than distribution.json ${sources_version}")
endif()
# Literal dots: a version or a suffix with another character in their place is another path.
string(REPLACE "." "[.]" header_pattern "${header_version}")
string(REPLACE "." "[.]" bootstrap_pattern "${bootstrap_VERSION}")
if(NOT header_environment MATCHES "/developer/${header_pattern}/environment[.]tgz$")
    message(FATAL_ERROR "DeveloperReleaseData.hpp is not the header of its version ${header_version}: ${header_environment}")
endif()
if(bootstrap_to_header GREATER 0)
    message(FATAL_ERROR "The bootstrap ${bootstrap_VERSION} is ahead of the header ${header_version}")
endif()
if(NOT bootstrap_URL MATCHES "/developer/${bootstrap_pattern}/environment[.]tgz$")
    message(FATAL_ERROR "The bootstrap URL is not the one of its version ${bootstrap_VERSION}: ${bootstrap_URL}")
endif()
if(bootstrap_to_header EQUAL 0
        AND (NOT bootstrap_URL STREQUAL header_environment OR NOT bootstrap_INTEGRITY STREQUAL header_integrity))
    message(FATAL_ERROR "The bootstrap and the header both name ${header_version} with a different URL or integrity")
endif()

# The origin of the pinned URL (ADR-021, decision 2, criterion 7), offline: on baseUrl, or under the declared migration
# waiver (previousBase with baseSince), which covers only a header whose version precedes baseSince. A waiver that the
# header has outgrown is stale: the release workflow removes it with the pin it generates, and this check, which it runs
# before anything is published, stops a release that would leave it.
string(JSON base_url GET "${distribution}" baseUrl)
if(NOT base_url MATCHES "/$")
    string(APPEND base_url "/")
endif()
string(JSON previous_base ERROR_VARIABLE no_previous GET "${distribution}" previousBase)
string(JSON base_since ERROR_VARIABLE no_since GET "${distribution}" baseSince)
if(no_previous AND NOT no_since)
    message(FATAL_ERROR "distribution.json has baseSince without previousBase")
elseif(NOT no_previous AND no_since)
    message(FATAL_ERROR "distribution.json has previousBase without baseSince, the first version published under baseUrl")
elseif(NOT no_previous)
    if(NOT previous_base MATCHES "/$")
        string(APPEND previous_base "/")
    endif()
    webfront_semver_compare("${header_version}" "${base_since}" header_to_base_since)
    string(FIND "${header_environment}" "${previous_base}" on_previous)
    if(NOT on_previous EQUAL 0 OR NOT header_to_base_since LESS 0)
        message(FATAL_ERROR "The migration waiver of distribution.json is stale: the header ${header_version} must be under "
            "${previous_base} and precede ${base_since}; remove previousBase and baseSince")
    endif()
else()
    string(FIND "${header_environment}" "${base_url}" on_base)
    if(NOT on_base EQUAL 0)
        message(FATAL_ERROR "The header pins ${header_environment}, which is not under baseUrl ${base_url}, and distribution.json "
            "declares no migration")
    endif()
endif()

if(header_to_sources EQUAL 0)
    set(state "(b) sources equal to the header")
else()
    set(state "(a) sources ${sources_version} ahead of the header")
endif()
message(STATUS "Developer pin: ${state}; header ${header_version}; bootstrap ${bootstrap_VERSION}")

# The "unreleased" row of the compatibility table follows the header (the release workflow rewrites it at each pin).
if(EXISTS "${ROOT}/docs/DEVELOPER_ENVIRONMENT.md")
    file(READ "${ROOT}/docs/DEVELOPER_ENVIRONMENT.md" environment_document)
    if(NOT environment_document MATCHES "\n[|] unreleased [|] ([^ |]+) [|]")
        message(FATAL_ERROR "The compatibility table has no unreleased row")
    endif()
    if(NOT "${CMAKE_MATCH_1}" STREQUAL "${header_version}")
        message(FATAL_ERROR "The unreleased row of the compatibility table names ${CMAKE_MATCH_1}, not the pinned ${header_version}")
    endif()
endif()
