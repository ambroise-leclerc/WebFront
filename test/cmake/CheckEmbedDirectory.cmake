# webfront_embed_directory() from a parent project (#675): a directory with nested folders, binary, empty and hidden files
# is embedded, built and served with the same bytes and MIME types as from the disk; then the build follows the directory:
# an unchanged directory regenerates nothing, a changed file regenerates the provider, and an added or removed file is
# served or no longer served after the next build, with no manual configure.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch> [-DCPM_MODULE=...] [-DDEPENDENCY_ARGS=...] [-DTEST_GENERATOR=...]
#         [-DTEST_CXX_COMPILER=...] [-DTEST_BUILD_TYPE=...] -P CheckEmbedDirectory.cmake
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
if(NOT TEST_BUILD_TYPE)
    set(TEST_BUILD_TYPE Release)
endif()

set(ui "${WORK_DIR}/ui dir")
set(build "${WORK_DIR}/build")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${ui}/lib/vendor" "${ui}/lib/@scope" "${ui}/assets" "${ui}/a/b/c" "${ui}/.hidden")

# Text, binary and empty files, word boundaries (40 and 41 bytes), names with a space, @, + and ~, and hidden files.
file(WRITE "${ui}/index.html" "<!doctype html><title>Embedded</title><script src=\"/WebFront.js\"></script><script type=\"module\" src=\"./app.js\"></script>\n")
file(WRITE "${ui}/app.js" "import {chart} from './lib/vendor/chart.min.js';\nchart();\n")
file(WRITE "${ui}/style.css" "body { margin: 0; }\n")
file(WRITE "${ui}/lib/vendor/chart.min.js" "export const chart = () => 1;\n")
file(WRITE "${ui}/lib/@scope/pkg+1~x.mjs" "export default 42;\n")
file(WRITE "${ui}/a/b/c/deep.json" "{\"deep\": true}\n")
file(WRITE "${ui}/a/forty.txt" "0123456789012345678901234567890123456789")
file(WRITE "${ui}/a/forty-one.csv" "01234567890123456789012345678901234567890")
file(WRITE "${ui}/empty.txt" "")
file(COPY_FILE "${ROOT}/js/core/favicon.ico" "${ui}/assets/icon.ico")
file(COPY_FILE "${ROOT}/docs/images/life-preview.jpg" "${ui}/assets/my image.jpg")
file(COPY_FILE "${ROOT}/js/core/favicon.ico" "${ui}/assets/module.wasm")
file(WRITE "${ui}/.DS_Store" "not served")
file(WRITE "${ui}/.hidden/secret.txt" "not served")

set(configure_args "")
if(TEST_GENERATOR)
    list(APPEND configure_args -G "${TEST_GENERATOR}")
endif()
if(TEST_CXX_COMPILER)
    list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}")
endif()
if(TEST_CXX_COMPILER_LAUNCHER)
    list(APPEND configure_args "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=${TEST_CXX_COMPILER_LAUNCHER}")
endif()
message(STATUS "Configuring the consumer")
execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args} -S "${ROOT}/test/cmake/embed-directory" -B "${build}"
    "-DWEBFRONT_SOURCE_DIR=${ROOT}" "-DUI_DIR=${ui}" "-DCPM_MODULE=${CPM_MODULE}" ${DEPENDENCY_ARGS}
    -DWEBFRONT_AUTODETECT_NATIVE=OFF -DENABLE_CACHE=OFF "-DCMAKE_BUILD_TYPE=${TEST_BUILD_TYPE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed:\n${output}${error}")
endif()

# Builds both consumers and returns the build's output, to tell whether the provider was regenerated.
function(build_consumer step out_output)
    message(STATUS "Building the consumer: ${step}")
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --config "${TEST_BUILD_TYPE}" --parallel
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Consumer build failed (${step}):\n${output}${error}")
    endif()
    set(${out_output} "${output}${error}" PARENT_SCOPE)
endfunction()

function(find_program_of name out_path)
    file(GLOB_RECURSE candidates "${build}/${name}" "${build}/${name}.exe")
    list(FILTER candidates EXCLUDE REGEX "/CMakeFiles/")
    list(LENGTH candidates count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "Expected one ${name} program in ${build}, found: ${candidates}")
    endif()
    set(${out_path} "${candidates}" PARENT_SCOPE)
endfunction()

# Runs both consumers; each serves every file and compares it with the disk.
function(serve step expected_files)
    foreach(name embed_consumer embed_default)
        find_program_of(${name} program)
        execute_process(COMMAND "${program}" "${ui}" ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name} (${step}) serves the directory differently from the disk:\n${output}${error}")
        endif()
        if(NOT output MATCHES "^${expected_files} files served identically")
            message(FATAL_ERROR "${name} (${step}) expected ${expected_files} files:\n${output}")
        endif()
        string(STRIP "${output}" output)
        message(STATUS "${name} (${step}): ${output}")
    endforeach()
endfunction()

# Make tools that compare whole seconds (GNU make 3.81 of macOS) would take an object built in the same second as a change
# for current: each change waits until the previous build is in an earlier second, as an edit by hand always is.
function(wait_for_next_second)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
endfunction()

build_consumer("first build" output)
if(NOT output MATCHES "Embedding .*ConsumerUI" OR NOT output MATCHES "Embedding .*webfront::fs::EmbeddedDirectory")
    message(FATAL_ERROR "The first build must generate both providers:\n${output}")
endif()
serve("first build" 12)

build_consumer("unchanged directory" output)
if(output MATCHES "Embedding ")
    message(FATAL_ERROR "An unchanged directory must not regenerate the provider:\n${output}")
endif()

# A changed file: the next build regenerates the provider, and the consumer, which compares with the disk, passes again.
wait_for_next_second()
file(APPEND "${ui}/lib/vendor/chart.min.js" "export const version = 2;\n")
build_consumer("changed file" output)
if(NOT output MATCHES "Embedding ")
    message(FATAL_ERROR "A changed file must regenerate the provider:\n${output}")
endif()
serve("changed file" 12)

# An added and a removed file: the glob is checked by the build, which configures again.
wait_for_next_second()
file(MAKE_DIRECTORY "${ui}/added")
file(WRITE "${ui}/added/new.svg" "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n")
file(REMOVE "${ui}/app.js")
build_consumer("added and removed files" output)
serve("added and removed files" 12 --absent app.js)
