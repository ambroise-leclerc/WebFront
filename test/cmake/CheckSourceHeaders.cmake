# SPDX-License-Identifier: Apache-2.0 OR CECILL-B
# Author: Ambroise Leclerc
# Created: 2026-11-24
#
# WebFront — tests/cmake
# Role: Checks that every tracked C++ source starts with the normalized file header (SPDX, Author, Created, subsystem, Role, Thread, Alloc, LLM).
# Thread: worker-safe | Alloc: unrestricted
# LLM: CMake script of the SourceHeaders CTest case; the format is documented in .github/CONTRIBUTING.md, keep both in step
#
#   cmake -DROOT=<checkout> [-DGIT_EXECUTABLE=<git>] -P test/cmake/CheckSourceHeaders.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

# Generated or fixed data keeps its own content: the embedded asset headers (js/core/EmbedAsset.cmake, wf build) and the gzip fixtures.
set(exempt
    include/webfront/system/DeveloperBootstrapData.hpp
    include/webfront/system/DeveloperIndexHtmlData.hpp
    include/webfront/system/DeveloperReleaseData.hpp
    include/webfront/system/DomInterpreterData.hpp
    include/webfront/system/FallbackIndexHtmlData.hpp
    include/webfront/system/FallbackModuleData.hpp
    include/webfront/system/FaviconData.hpp
    include/webfront/system/WebFrontJsData.hpp
    test/unit/GzipFixtures.hpp)

set(sources "")
set(from_git OFF)
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE git)
endif()
if(GIT_EXECUTABLE AND EXISTS "${ROOT}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files
        WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE tracked RESULT_VARIABLE git_result ERROR_QUIET)
    if(git_result EQUAL 0)
        set(from_git ON)
        string(REPLACE "\n" ";" sources "${tracked}")
    endif()
endif()
if(NOT from_git)
    file(GLOB_RECURSE sources RELATIVE "${ROOT}" "${ROOT}/include/*" "${ROOT}/test/*" "${ROOT}/examples/*" "${ROOT}/cmake/*")
endif()
list(FILTER sources INCLUDE REGEX "\\.(hpp|cpp|h|cu|cuh|mm)$")
list(REMOVE_ITEM sources ${exempt})

set(thread_values "ui-loop|bridge-io|worker-safe|thread-safe")
set(alloc_values "zero-hot-path|heap-init|unrestricted")
set(problems "")
set(checked 0)
foreach(source IN LISTS sources)
    if(NOT EXISTS "${ROOT}/${source}")
        continue()
    endif()
    math(EXPR checked "${checked} + 1")
    file(STRINGS "${ROOT}/${source}" head LIMIT_COUNT 8 ENCODING UTF-8)
    list(LENGTH head count)
    if(count LESS 8)
        list(APPEND problems "${source}: fewer than 8 lines, the header is missing")
        continue()
    endif()
    list(GET head 0 l1)
    list(GET head 1 l2)
    list(GET head 2 l3)
    list(GET head 3 l4)
    list(GET head 4 l5)
    list(GET head 5 l6)
    list(GET head 6 l7)
    list(GET head 7 l8)
    string(REGEX REPLACE "\r$" "" l1 "${l1}")
    if(NOT l1 STREQUAL "// SPDX-License-Identifier: Apache-2.0 OR CECILL-B")
        list(APPEND problems "${source}: line 1 must be '// SPDX-License-Identifier: Apache-2.0 OR CECILL-B'")
        continue()
    endif()
    if(NOT l2 MATCHES "^// Author: [^ ].*$")
        list(APPEND problems "${source}: line 2 must be '// Author: <name>'")
    endif()
    if(NOT l3 MATCHES "^// Created: ([0-9][0-9][0-9][0-9])-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])$")
        list(APPEND problems "${source}: line 3 must be '// Created: AAAA-MM-JJ' with a valid date, found '${l3}'")
    endif()
    if(NOT l4 STREQUAL "//")
        list(APPEND problems "${source}: line 4 must be '//'")
    endif()
    if(NOT l5 MATCHES "^// WebFront — [^ ].*$")
        list(APPEND problems "${source}: line 5 must be '// WebFront — <subsystem>'")
    endif()
    if(NOT l6 MATCHES "^// Role: [^ ].*$")
        list(APPEND problems "${source}: line 6 must be '// Role: <responsibility>'")
    endif()
    if(NOT l7 MATCHES "^// Thread: (${thread_values}) \\| Alloc: (${alloc_values})$")
        list(APPEND problems "${source}: line 7 must be '// Thread: <${thread_values}> | Alloc: <${alloc_values}>', found '${l7}'")
    endif()
    if(NOT l8 MATCHES "^// LLM: [^ ].*$")
        list(APPEND problems "${source}: line 8 must be '// LLM: <reading hint>'")
    endif()
endforeach()

if(checked EQUAL 0)
    message(FATAL_ERROR "No C++ source found under ${ROOT}")
endif()
if(problems)
    string(REPLACE ";" "\n  " report "${problems}")
    message(FATAL_ERROR "File headers drifted from the normalized format (${checked} files checked):\n  ${report}")
endif()
message(STATUS "${checked} file headers conform")
