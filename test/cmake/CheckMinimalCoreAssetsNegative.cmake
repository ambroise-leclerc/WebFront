# The guard of the minimal core must fail on an unlisted resource header and pass once it is removed.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch> -P test/cmake/CheckMinimalCoreAssetsNegative.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/include")
file(COPY "${ROOT}/include/webfront" DESTINATION "${WORK_DIR}/include")

function(run_guard result_variable output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DROOT=${WORK_DIR} -P "${CMAKE_CURRENT_LIST_DIR}/CheckMinimalCoreAssets.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

run_guard(clean_result clean_output)
if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR "The guard rejects a clean copy of the headers:\n${clean_output}")
endif()

set(rogue "${WORK_DIR}/include/webfront/system/RogueData.hpp")
file(WRITE "${rogue}" "#pragma once\n// not a normative resource\n")
run_guard(rogue_result rogue_output)
if(rogue_result EQUAL 0 OR NOT rogue_output MATCHES "system/RogueData\\.hpp")
    message(FATAL_ERROR "The guard accepts an unlisted *Data.hpp:\n${rogue_output}")
endif()

# A payload hidden behind another name is a resource too.
file(REMOVE "${rogue}")
file(WRITE "${WORK_DIR}/include/webfront/system/Sneaky.hpp"
    "#pragma once\nstruct Sneaky { static constexpr unsigned long dataSize{1}; };\n")
run_guard(sneaky_result sneaky_output)
if(sneaky_result EQUAL 0 OR NOT sneaky_output MATCHES "system/Sneaky\\.hpp")
    message(FATAL_ERROR "The guard accepts an unlisted payload header:\n${sneaky_output}")
endif()

# A resource that outgrows the budget is refused as well.
file(REMOVE "${WORK_DIR}/include/webfront/system/Sneaky.hpp")
file(WRITE "${WORK_DIR}/include/webfront/system/FaviconData.hpp"
    "#pragma once\nstruct FaviconData { static constexpr std::string_view encoding{}; static constexpr std::size_t dataSize{999999}; };\n")
run_guard(budget_result budget_output)
if(budget_result EQUAL 0 OR NOT budget_output MATCHES "over the budget")
    message(FATAL_ERROR "The guard accepts an oversized resource:\n${budget_output}")
endif()

# The budget counts decoded bytes: a small gzip stream whose trailer announces 196608 bytes is over it.
file(WRITE "${WORK_DIR}/include/webfront/system/FaviconData.hpp"
    "#pragma once\nstruct FaviconData {\n    static constexpr std::string_view encoding{\"gzip\"};\n"
    "    static constexpr std::size_t dataSize{8};\n    static constexpr std::array<std::uint64_t, 1> data{ 0x0000000000000300 };\n};\n")
run_guard(gzip_result gzip_output)
if(gzip_result EQUAL 0 OR NOT gzip_output MATCHES "gzip, 196608 decoded" OR NOT gzip_output MATCHES "over the budget")
    message(FATAL_ERROR "The guard ignores the decoded size of a gzip resource:\n${gzip_output}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
