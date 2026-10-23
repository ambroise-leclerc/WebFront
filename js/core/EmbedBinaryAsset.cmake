# Renders a small binary asset (identity encoded) as a header in the format of the embedded assets,
# in pure CMake: no Node.js, no script. Bounded lines are joined once, so the string never grows
# word by word. Used for the default favicon; larger or compressed assets are out of scope.
#   cmake -DINPUT=<file> -DSTRUCT=<name> (-DOUTPUT=<header> | -DCHECK=<header>) -P EmbedBinaryAsset.cmake
if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT must name the binary asset source")
endif()
if(NOT DEFINED STRUCT)
    message(FATAL_ERROR "STRUCT must name the generated struct, e.g. FaviconData")
endif()
if(NOT DEFINED OUTPUT AND NOT DEFINED CHECK)
    message(FATAL_ERROR "Set OUTPUT to regenerate the header or CHECK to verify it")
endif()

file(SIZE "${INPUT}" size)
file(READ "${INPUT}" hex HEX)
math(EXPR word_count "(${size} + 7) / 8")

set(lines "")
string(LENGTH "${hex}" hex_length)
set(offset 0)
while(offset LESS hex_length)
    math(EXPR remaining "${hex_length} - ${offset}")
    if(remaining GREATER 80)
        set(remaining 80)
    endif()
    string(SUBSTRING "${hex}" ${offset} ${remaining} chunk)
    string(REGEX MATCHALL ".[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?[^;]?" words "${chunk}")
    set(rendered "")
    foreach(word IN LISTS words)
        string(LENGTH "${word}" word_length)
        math(EXPR missing "16 - ${word_length}")
        string(REPEAT "0" ${missing} padding)
        list(APPEND rendered "0x${word}${padding}")
    endforeach()
    list(JOIN rendered ", " line)
    list(APPEND lines "        ${line}")
    math(EXPR offset "${offset} + 80")
endwhile()
list(JOIN lines ",\n" body)
if(body)
    set(body "\n${body}")
endif()

set(contents "#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace webfront::fs::generated {

struct ${STRUCT} {
    static constexpr std::string_view encoding{};
    static constexpr std::size_t dataSize{${size}};
    static constexpr std::array<std::uint64_t, ${word_count}> data{${body}
    };
};

} // namespace webfront::fs::generated
")

if(DEFINED CHECK)
    if(NOT EXISTS "${CHECK}")
        message(FATAL_ERROR "${CHECK} is missing; run the webfront-embedded-assets target")
    endif()
    file(READ "${CHECK}" current)
    if(NOT current STREQUAL contents)
        message(FATAL_ERROR "${CHECK} is stale; run the webfront-embedded-assets target")
    endif()
else()
    get_filename_component(directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${OUTPUT}" "${contents}")
endif()
