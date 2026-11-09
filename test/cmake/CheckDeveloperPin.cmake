# Offline consistency of the developer pin (ADR-021, decision 2), in pure CMake. The three places that name a version:
#   distribution.json         the sources' version (may be ahead of the pin: state (a))
#   DeveloperReleaseData.hpp  the version applications accept
#   WebFrontBootstrap.cmake   the version the tests run (the header's, or an earlier published one: state (c))
# Normal: distribution == header or ahead of it; bootstrap <= header. Inconsistent: a header newer than distribution.json, a
# header whose URL is not the one of its own version, a bootstrap newer than the header, or a bootstrap equal to the header
# with another URL or integrity. The bytes and the origin of the pinned URL are checked by `wf run pin`; a header equal to a
# fresh build, by the comparison of the Developer packages workflow. ROOT is the repository (or a fixture of it).
cmake_minimum_required(VERSION 3.31)
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

if(header_version VERSION_GREATER sources_version)
    message(FATAL_ERROR "DeveloperReleaseData.hpp pins ${header_version}, newer than distribution.json ${sources_version}")
endif()
# Literal dots: a version or a suffix with another character in their place is another path.
string(REPLACE "." "[.]" header_pattern "${header_version}")
string(REPLACE "." "[.]" bootstrap_pattern "${bootstrap_VERSION}")
if(NOT header_environment MATCHES "/developer/${header_pattern}/environment[.]tgz$")
    message(FATAL_ERROR "DeveloperReleaseData.hpp is not the header of its version ${header_version}: ${header_environment}")
endif()
if(bootstrap_VERSION VERSION_GREATER header_version)
    message(FATAL_ERROR "The bootstrap ${bootstrap_VERSION} is ahead of the header ${header_version}")
endif()
if(NOT bootstrap_URL MATCHES "/developer/${bootstrap_pattern}/environment[.]tgz$")
    message(FATAL_ERROR "The bootstrap URL is not the one of its version ${bootstrap_VERSION}: ${bootstrap_URL}")
endif()
if(bootstrap_VERSION VERSION_EQUAL header_version
        AND (NOT bootstrap_URL STREQUAL header_environment OR NOT bootstrap_INTEGRITY STREQUAL header_integrity))
    message(FATAL_ERROR "The bootstrap and the header both name ${header_version} with a different URL or integrity")
endif()

if(header_version VERSION_EQUAL sources_version)
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
