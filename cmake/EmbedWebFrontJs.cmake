if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT must name the readable WebFront.js source")
endif()

if(NOT DEFINED OUTPUT AND NOT DEFINED CHECK)
    message(FATAL_ERROR "Set OUTPUT to regenerate the header or CHECK to verify it")
endif()

set(gzip_file "${CMAKE_CURRENT_BINARY_DIR}/WebFront.js.gz")
file(ARCHIVE_CREATE
    OUTPUT "${gzip_file}"
    PATHS "${INPUT}"
    FORMAT raw
    COMPRESSION GZip
    COMPRESSION_LEVEL 9
    MTIME 0
)
file(READ "${gzip_file}" gzip_hex HEX)
file(REMOVE "${gzip_file}")

# libarchive currently records platform metadata in the gzip header even when
# ARCHIVE_CREATE receives MTIME 0. Normalize bytes 4-7 (timestamp) and byte 9
# (originating OS); they are not covered by the compressed stream checksum.
string(SUBSTRING "${gzip_hex}" 0 8 gzip_prefix)
string(SUBSTRING "${gzip_hex}" 16 2 gzip_extra_flags)
string(SUBSTRING "${gzip_hex}" 20 -1 gzip_payload)
set(gzip_hex "${gzip_prefix}00000000${gzip_extra_flags}ff${gzip_payload}")

string(LENGTH "${gzip_hex}" hex_length)
math(EXPR data_size "${hex_length} / 2")
math(EXPR word_count "(${data_size} + 7) / 8")

set(words "")
set(word_index 0)
while(word_index LESS word_count)
    math(EXPR offset "${word_index} * 16")
    math(EXPR remaining "${hex_length} - ${offset}")
    if(remaining GREATER 16)
        set(chunk_length 16)
    else()
        set(chunk_length "${remaining}")
    endif()
    string(SUBSTRING "${gzip_hex}" "${offset}" "${chunk_length}" word)
    string(LENGTH "${word}" word_length)
    while(word_length LESS 16)
        string(APPEND word "0")
        string(LENGTH "${word}" word_length)
    endwhile()
    math(EXPR line_position "${word_index} % 5")
    if(line_position EQUAL 0)
        if(word_index GREATER 0)
            string(APPEND words ",")
        endif()
        string(APPEND words "\n        ")
    elseif(word_index GREATER 0)
        string(APPEND words ", ")
    endif()
    string(APPEND words "0x${word}")
    math(EXPR word_index "${word_index} + 1")
endwhile()

set(contents "#pragma once\n\n#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <string_view>\n\nnamespace webfront::fs::generated {\n\nstruct WebFrontJsData {\n    static constexpr std::string_view encoding{\"gzip\"};\n    static constexpr std::size_t dataSize{${data_size}};\n    static constexpr std::array<std::uint64_t, ${word_count}> data{${words}\n    };\n};\n\n} // namespace webfront::fs::generated\n")

if(DEFINED CHECK)
    file(READ "${CHECK}" expected)
    if(NOT contents STREQUAL expected)
        message(FATAL_ERROR "${CHECK} is stale; run the webfront-js-assets target")
    endif()
else()
    file(WRITE "${OUTPUT}" "${contents}")
endif()
