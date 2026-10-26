# The renderer of the embedded assets (js/core/EmbedAsset.cmake): the exact format of a header, word order and padding, the
# check of exact bytes, the exit status of a stale or missing header, a path with spaces and a checkout reached through a
# symbolic link.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch directory> -P test/cmake/CheckEmbedAsset.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(script "${ROOT}/js/core/EmbedAsset.cmake")

# render(<status variable> <error text variable> <input> <arguments>...): runs the script like the build does.
function(render status errors script_path input)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DINPUT=${input}" -DSTRUCT=Fixture ${ARGN} -P "${script_path}"
        WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE text)
    set(${status} "${result}" PARENT_SCOPE)
    set(${errors} "${text}" PARENT_SCOPE)
endfunction()

function(expect_status expected description)
    if(NOT status EQUAL expected)
        message(FATAL_ERROR "${description}: expected exit status ${expected}, got ${status}\n${errors}")
    endif()
endfunction()
function(expect_stale description)
    expect_status(1 "${description}")
    # CMake wraps the text of an error at the width of the terminal, wherever the path of the header leaves room.
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${errors}")
    if(NOT flat MATCHES "is stale; run the webfront-embedded-assets target")
        message(FATAL_ERROR "${description}: no stale message:\n${errors}")
    endif()
endfunction()

# The format, word order, padding and line wrapping, byte for byte (41 bytes: five full words and one padded).
file(WRITE "${WORK_DIR}/input 41.txt" "0123456789abcdefghijklmnopqrstuvwxyzABCDE")
render(status errors "${script}" "${WORK_DIR}/input 41.txt" "-DOUTPUT=${WORK_DIR}/nested/Fixture.hpp")
expect_status(0 "rendering a 41-byte input")
file(READ "${WORK_DIR}/nested/Fixture.hpp" rendered)
set(expected "#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace webfront::fs::generated {

struct Fixture {
    static constexpr std::string_view encoding{};
    static constexpr std::size_t dataSize{41};
    static constexpr std::array<std::uint64_t, 6> data{
        0x3031323334353637, 0x3839616263646566, 0x6768696a6b6c6d6e, 0x6f70717273747576, 0x7778797a41424344,
        0x4500000000000000
    };
};

} // namespace webfront::fs::generated
")
if(NOT rendered STREQUAL expected)
    message(FATAL_ERROR "The rendered header differs from the expected format:\n${rendered}")
endif()

# Padding and the empty array, for the sizes around the word and line boundaries.
string(REPEAT "x" 41 filler)
foreach(size 0 1 7 8 9 39 40 41)
    string(SUBSTRING "${filler}" 0 ${size} content)
    file(WRITE "${WORK_DIR}/sized.bin" "${content}")
    render(status errors "${script}" "${WORK_DIR}/sized.bin" "-DOUTPUT=${WORK_DIR}/sized.hpp")
    expect_status(0 "rendering ${size} bytes")
    file(READ "${WORK_DIR}/sized.hpp" header)
    math(EXPR words "(${size} + 7) / 8")
    if(NOT header MATCHES "dataSize\\{${size}\\}" OR NOT header MATCHES "std::array<std::uint64_t, ${words}> data\\{")
        message(FATAL_ERROR "Wrong size or array length for ${size} bytes:\n${header}")
    endif()
    # The words, read back, are the content followed by zero padding only.
    string(REGEX MATCHALL "0x[0-9a-f]+" found "${header}")
    string(REPLACE "0x" "" found "${found}")
    string(REPLACE ";" "" decoded "${found}")
    string(HEX "${content}" hex)
    string(LENGTH "${decoded}" decoded_length)
    math(EXPR padded "${words} * 16")
    if(NOT decoded_length EQUAL padded OR NOT decoded MATCHES "^${hex}0*$")
        message(FATAL_ERROR "The words of ${size} bytes do not decode to the input: ${decoded}")
    endif()
    if(size EQUAL 0 AND NOT header MATCHES "data\\{\n    };")
        message(FATAL_ERROR "An empty asset must render an empty array:\n${header}")
    endif()
endforeach()

# Exact bytes: a current header passes, a stale or CRLF one, a changed source or a missing header fail. The favicon is binary.
foreach(source "${WORK_DIR}/input 41.txt" "${ROOT}/js/core/favicon.ico")
    render(status errors "${script}" "${source}" "-DOUTPUT=${WORK_DIR}/Checked.hpp")
    expect_status(0 "rendering ${source}")
    render(status errors "${script}" "${source}" "-DCHECK=${WORK_DIR}/Checked.hpp")
    expect_status(0 "checking a current header")
    file(READ "${WORK_DIR}/Checked.hpp" current)
    string(REPLACE "\n" "\r\n" crlf "${current}")
    file(WRITE "${WORK_DIR}/Checked.hpp" "${crlf}")
    render(status errors "${script}" "${source}" "-DCHECK=${WORK_DIR}/Checked.hpp")
    if(status EQUAL 0)
        message(FATAL_ERROR "A header with CRLF line ends must be stale")
    endif()
    file(WRITE "${WORK_DIR}/Checked.hpp" "stale header")
    render(status errors "${script}" "${source}" "-DCHECK=${WORK_DIR}/Checked.hpp")
    expect_stale("checking a stale header")
    file(REMOVE "${WORK_DIR}/Checked.hpp")
    render(status errors "${script}" "${source}" "-DCHECK=${WORK_DIR}/Checked.hpp")
    if(status EQUAL 0)
        message(FATAL_ERROR "A missing header must fail the check")
    endif()
endforeach()
file(WRITE "${WORK_DIR}/changing.txt" "before")
render(status errors "${script}" "${WORK_DIR}/changing.txt" "-DOUTPUT=${WORK_DIR}/Changing.hpp")
file(WRITE "${WORK_DIR}/changing.txt" "after")
render(status errors "${script}" "${WORK_DIR}/changing.txt" "-DCHECK=${WORK_DIR}/Changing.hpp")
expect_stale("checking a header after its source changed")

# Missing arguments and a missing source are errors, never an empty header.
foreach(arguments "-DOUTPUT=${WORK_DIR}/none.hpp" "-DCHECK=${WORK_DIR}/Changing.hpp")
    render(status errors "${script}" "${WORK_DIR}/absent.txt" ${arguments})
    if(status EQUAL 0)
        message(FATAL_ERROR "A missing source must fail (${arguments})")
    endif()
endforeach()
if(EXISTS "${WORK_DIR}/none.hpp")
    message(FATAL_ERROR "A missing source must not write a header")
endif()
render(status errors "${script}" "${WORK_DIR}/changing.txt")
if(status EQUAL 0)
    message(FATAL_ERROR "Neither OUTPUT nor CHECK must fail")
endif()

# The checkout reached through a symbolic link (skipped where the platform refuses to create one).
file(CREATE_LINK "${ROOT}" "${WORK_DIR}/linked checkout" SYMBOLIC RESULT linked)
if(linked STREQUAL "0")
    foreach(argument "-DOUTPUT=${WORK_DIR}/Linked.hpp" "-DCHECK=${WORK_DIR}/Linked.hpp")
        render(status errors "${WORK_DIR}/linked checkout/js/core/EmbedAsset.cmake" "${WORK_DIR}/input 41.txt" "${argument}")
        expect_status(0 "through a symbolic link (${argument})")
    endforeach()
else()
    message(STATUS "Symbolic links are not available: ${linked}")
endif()
