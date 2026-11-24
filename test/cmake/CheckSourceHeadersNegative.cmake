# SPDX-License-Identifier: Apache-2.0 OR CECILL-B
# Author: Ambroise Leclerc
# Created: 2026-11-24
#
# WebFront — tests/cmake
# Role: Proves that the file header guard accepts a conforming source and rejects a missing, malformed or out-of-list header.
# Thread: worker-safe | Alloc: unrestricted
# LLM: CMake script of the SourceHeadersRejectsDrift CTest case; it builds a scratch tree without Git metadata, so the guard scans the directories
#
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch> -P test/cmake/CheckSourceHeadersNegative.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")

set(good_header
"// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-24
//
// WebFront — tests/cmake
// Role: A conforming header.
// Thread: worker-safe | Alloc: heap-init
// LLM: scratch file of the negative test
")

function(run_guard result_variable output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DROOT=${WORK_DIR} -P "${CMAKE_CURRENT_LIST_DIR}/CheckSourceHeaders.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

file(WRITE "${WORK_DIR}/include/webfront/Good.hpp" "${good_header}\n#pragma once\n")
run_guard(clean_result clean_output)
if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR "The guard rejects a conforming header:\n${clean_output}")
endif()

# Each drift is written alone, checked, then removed.
function(expect_rejection name content expected)
    file(WRITE "${WORK_DIR}/test/unit/${name}.cpp" "${content}")
    run_guard(result output)
    if(result EQUAL 0 OR NOT output MATCHES "${expected}")
        message(FATAL_ERROR "The guard accepts ${name}:\n${output}")
    endif()
    file(REMOVE "${WORK_DIR}/test/unit/${name}.cpp")
endfunction()

expect_rejection(NoHeader "#include <string>\nint main() {}\nint x;\nint y;\nint z;\nint a;\nint b;\nint c;\n" "line 1 must be")
string(REPLACE "Created: 2026-11-24" "Created: 24/11/2026" bad_date "${good_header}")
expect_rejection(BadDate "${bad_date}" "Created: AAAA-MM-JJ")
string(REPLACE "Created: 2026-11-24" "Created: 2026-13-45" bad_month "${good_header}")
expect_rejection(BadMonth "${bad_month}" "valid date")
string(REPLACE "Thread: worker-safe" "Thread: any" bad_thread "${good_header}")
expect_rejection(BadThread "${bad_thread}" "Thread:")
string(REPLACE "Alloc: heap-init" "Alloc: whatever" bad_alloc "${good_header}")
expect_rejection(BadAlloc "${bad_alloc}" "Alloc:")
string(REPLACE "// Role: A conforming header.\n" "" no_role "${good_header}")
expect_rejection(NoRole "${no_role}x\n" "line 6 must be")
string(REPLACE "// LLM: scratch file of the negative test\n" "// LLM:\n" empty_llm "${good_header}")
expect_rejection(EmptyLlm "${empty_llm}" "LLM")
