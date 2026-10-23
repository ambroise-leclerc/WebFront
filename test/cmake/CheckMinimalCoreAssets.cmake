# Guard of the minimal core (ADR-016): the generated resource headers of include/webfront/ are a
# closed, normative list with a size budget. Adding a web resource means changing this list and the
# ADR in the same change.
#   cmake -DROOT=<checkout> -P test/cmake/CheckMinimalCoreAssets.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

# Normative list: header file name under include/webfront/system/.
set(allowed
    WebFrontJsData.hpp          # bridge WebFront.js
    FallbackIndexHtmlData.hpp   # fallback page
    FallbackModuleData.hpp      # fallback module
    FaviconData.hpp             # default favicon
    DeveloperIndexHtmlData.hpp  # bootstrap page
    DeveloperBootstrapData.hpp  # bootstrap loader
    DeveloperReleaseData.hpp)   # pinned release (URL and SHA-512, no payload)
# Budget on the decoded bytes of the embedded payloads, independent of their encoding.
set(budget_bytes 153600)

file(GLOB_RECURSE headers RELATIVE "${ROOT}/include/webfront" "${ROOT}/include/webfront/*.hpp")
list(SORT headers)

set(generated "")
foreach(header IN LISTS headers)
    get_filename_component(name "${header}" NAME)
    file(READ "${ROOT}/include/webfront/${header}" text)
    # A resource header is named *Data.hpp or embeds a payload (a dataSize constant).
    if(name MATCHES "Data\\.hpp$" OR text MATCHES "dataSize[ \t]*[{=]")
        list(APPEND generated "${header}")
    endif()
endforeach()

set(unexpected "")
set(decoded_total 0)
set(encoded_total 0)
set(file_total 0)
foreach(header IN LISTS generated)
    get_filename_component(name "${header}" NAME)
    if(NOT header STREQUAL "system/${name}" OR NOT name IN_LIST allowed)
        list(APPEND unexpected "${header}")
        continue()
    endif()
    file(SIZE "${ROOT}/include/webfront/${header}" file_size)
    math(EXPR file_total "${file_total} + ${file_size}")
    file(READ "${ROOT}/include/webfront/${header}" text)
    if(NOT text MATCHES "dataSize\\{([0-9]+)\\}")
        set(encoded 0)
        set(decoded 0)
        set(kind "no payload")
    else()
        set(encoded ${CMAKE_MATCH_1})
        set(decoded ${encoded})
        set(kind "identity")
        if(text MATCHES "encoding\\{\"gzip\"\\}")
            # The decoded size is the ISIZE trailer: the last four bytes, little endian.
            string(REGEX MATCHALL "0x[0-9a-f]+" words "${text}")
            string(REGEX REPLACE "0x" "" words "${words}")
            string(REPLACE ";" "" hex "${words}")
            math(EXPR trailer "(${encoded} - 4) * 2")
            string(SUBSTRING "${hex}" ${trailer} 8 isize)
            string(REGEX REPLACE "^(..)(..)(..)(..)$" "0x\\4\\3\\2\\1" isize "${isize}")
            math(EXPR decoded "${isize}")
            set(kind "gzip")
        endif()
    endif()
    math(EXPR decoded_total "${decoded_total} + ${decoded}")
    math(EXPR encoded_total "${encoded_total} + ${encoded}")
    message(STATUS "${header}: ${kind}, ${decoded} decoded, ${encoded} encoded bytes")
endforeach()

# Every listed resource must exist: a stale list is as wrong as a stale directory.
set(missing "")
foreach(name IN LISTS allowed)
    if(NOT EXISTS "${ROOT}/include/webfront/system/${name}")
        list(APPEND missing "${name}")
    endif()
endforeach()

message(STATUS "Embedded resources: ${decoded_total} decoded, ${encoded_total} encoded bytes, "
    "${file_total} bytes of headers (budget ${budget_bytes} decoded)")

if(unexpected)
    list(JOIN unexpected "\n  " list)
    message(FATAL_ERROR "Resource headers outside the normative list of ADR-016:\n  ${list}\n"
        "Remove them, or amend the ADR and test/cmake/CheckMinimalCoreAssets.cmake together.")
endif()
if(missing)
    list(JOIN missing "\n  " list)
    message(FATAL_ERROR "Listed resource headers are missing:\n  ${list}")
endif()
if(decoded_total GREATER budget_bytes)
    message(FATAL_ERROR "Embedded resources total ${decoded_total} decoded bytes, over the budget of ${budget_bytes}")
endif()
