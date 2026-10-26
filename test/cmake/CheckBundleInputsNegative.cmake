# CheckBundleInputs.cmake must fail on an input that changed since the bundles were built, and on a checksum nobody can
# match, while it passes on an untouched copy.
#   cmake -DROOT=<checkout> -DWORK_DIR=<scratch directory> -P test/cmake/CheckBundleInputsNegative.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(READ "${ROOT}/js/developer/tools/bundle-inputs.json" listed)
string(JSON count LENGTH "${listed}")
math(EXPR last "${count} - 1")
foreach(index RANGE 0 ${last})
    string(JSON path GET "${listed}" ${index})
    get_filename_component(directory "${path}" DIRECTORY)
    file(COPY "${ROOT}/${path}" DESTINATION "${WORK_DIR}/${directory}")
endforeach()
file(COPY "${ROOT}/js/developer/tools/bundle-inputs.json" DESTINATION "${WORK_DIR}/js/developer/tools")
file(COPY "${ROOT}/js/developer/environment/bundles.json" DESTINATION "${WORK_DIR}/js/developer/environment")

function(run_check expected_status description)
    execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${WORK_DIR} -P "${CMAKE_CURRENT_LIST_DIR}/CheckBundleInputs.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT status EQUAL expected_status)
        message(FATAL_ERROR "${description}: expected status ${expected_status}, got ${status}\n${output}${errors}")
    endif()
    set(text "${output}${errors}" PARENT_SCOPE)
endfunction()

run_check(0 "an untouched copy")

# A source modified after the bundles were built.
file(APPEND "${WORK_DIR}/js/developer/environment/restore.mjs" "// changed\n")
run_check(1 "a modified source")
if(NOT text MATCHES "environment/restore\\.mjs")
    message(FATAL_ERROR "The modified input is not named:\n${text}")
endif()
file(COPY "${ROOT}/js/developer/environment/restore.mjs" DESTINATION "${WORK_DIR}/js/developer/environment")
run_check(0 "a restored source")

# A bundle modified by hand.
file(APPEND "${WORK_DIR}/js/core/bootstrap/bootstrap.bundle.mjs" "// changed\n")
run_check(1 "a modified bundle")
if(NOT text MATCHES "bootstrap\\.bundle\\.mjs")
    message(FATAL_ERROR "The modified bundle is not named:\n${text}")
endif()
