# Renders an embedded asset of the core (identity encoded: the sources are small) as a header in the format of the embedded
# assets, in pure CMake: no other tool is needed to regenerate or check the headers of include/webfront/system. Bounded lines
# are joined once, so the string never grows word by word.
#   cmake -DINPUT=<file> -DSTRUCT=<name> (-DOUTPUT=<header> | -DCHECK=<header>) [-DGIT_EXECUTABLE=<git>] -P EmbedAsset.cmake
# CHECK compares the exact bytes of the header with the rendering of INPUT and fails when it is stale or missing.
if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT must name the asset source")
endif()
if(NOT DEFINED STRUCT)
    message(FATAL_ERROR "STRUCT must name the generated struct, e.g. WebFrontJsData")
endif()
if(NOT DEFINED OUTPUT AND NOT DEFINED CHECK)
    message(FATAL_ERROR "Set OUTPUT to regenerate the header or CHECK to verify it")
endif()
if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "${INPUT} does not exist")
endif()

# A Windows checkout converting either file to CRLF would make it look stale, so both must keep LF through
# .gitattributes. Checked only in a git work tree, on every platform; the favicon is binary.
if(DEFINED CHECK AND GIT_EXECUTABLE AND NOT INPUT MATCHES "\\.ico$")
    get_filename_component(source_dir "${INPUT}" DIRECTORY)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" check-attr eol -- "${INPUT}" "${CHECK}"
        WORKING_DIRECTORY "${source_dir}"
        OUTPUT_VARIABLE attributes
        RESULT_VARIABLE not_in_work_tree
        ERROR_QUIET)
    if(NOT not_in_work_tree)
        string(REGEX MATCHALL "eol: lf\n" lf_lines "${attributes}")
        list(LENGTH lf_lines lf_count)
        if(NOT lf_count EQUAL 2)
            message(FATAL_ERROR "${INPUT} and ${CHECK} must have 'text eol=lf' in .gitattributes:\n${attributes}")
        endif()
    endif()
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
    # Compared as bytes: a header with CRLF line ends is stale, which a text comparison would not see.
    file(READ "${CHECK}" current HEX)
    string(HEX "${contents}" expected)
    if(NOT current STREQUAL expected)
        message(FATAL_ERROR "${CHECK} is stale; run the webfront-embedded-assets target")
    endif()
else()
    get_filename_component(directory "${OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    # file(WRITE) opens the file in text mode, which writes CRLF on Windows: file(CONFIGURE) writes the line ends it is told to.
    # The header holds no @ and no ${...} that it could substitute.
    file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "${contents}" @ONLY NEWLINE_STYLE UNIX)
endif()
