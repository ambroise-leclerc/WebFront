if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT must name the readable text asset source")
endif()

if(NOT DEFINED OUTPUT AND NOT DEFINED CHECK)
    message(FATAL_ERROR "Set OUTPUT to regenerate the header or CHECK to verify it")
endif()

if(NOT DEFINED STRUCT)
    message(FATAL_ERROR "STRUCT must name the generated struct, e.g. WebFrontJsData")
endif()

# A Windows checkout converting either file to CRLF would make it look stale, so both must keep
# LF through .gitattributes. Checked only in a git work tree, on every platform.
if(DEFINED CHECK AND GIT_EXECUTABLE AND NOT INPUT MATCHES "\\.png$")
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

# Render and compare in one process without intermediate gzip/header files. Normal C++
# builds consume checked-in headers; Node is required only for maintenance and tests.
find_program(NODE_EXECUTABLE NAMES node nodejs REQUIRED)
set(arguments --input "${INPUT}" --struct "${STRUCT}")
if(GZIP)
    list(APPEND arguments --gzip)
endif()
if(DEFINED CHECK)
    list(APPEND arguments --check "${CHECK}")
else()
    list(APPEND arguments --output "${OUTPUT}")
endif()
execute_process(COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/assets/embed.mjs"
    ${arguments} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Embedded asset generation/check failed for ${INPUT}")
endif()
