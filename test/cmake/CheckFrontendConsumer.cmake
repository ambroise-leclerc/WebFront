cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()
set(configure_args)
if(TEST_GENERATOR)
    list(APPEND configure_args -G "${TEST_GENERATOR}")
endif()
if(TEST_CXX_COMPILER)
    list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${TEST_CXX_COMPILER}")
endif()
if(TEST_GENERATOR_PLATFORM)
    list(APPEND configure_args -A "${TEST_GENERATOR_PLATFORM}")
endif()
if(TEST_GENERATOR_TOOLSET)
    list(APPEND configure_args -T "${TEST_GENERATOR_TOOLSET}")
endif()
foreach(probe ON OFF)
    foreach(full_project OFF ON)
        message(STATUS "Consumer configure: probe=${probe}, full_project=${full_project}")
        execute_process(COMMAND "${CMAKE_COMMAND}" --fresh ${configure_args}
            -S "${ROOT}/test/cmake/frontend-consumer" -B "${WORK_DIR}/${probe}-${full_project}"
            "-DROOT=${ROOT}" "-DPROBE=${probe}" "-DFULL_PROJECT=${full_project}"
            "-DCPM_MODULE=${CPM_MODULE}" ${DEPENDENCY_ARGS}
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Consumer native probe ${probe}: ${output}${error}")
        endif()
        if(TEST_GENERATOR)
            file(STRINGS "${WORK_DIR}/${probe}-${full_project}/CMakeCache.txt" generator REGEX "^CMAKE_GENERATOR:INTERNAL=")
            if(NOT generator STREQUAL "CMAKE_GENERATOR:INTERNAL=${TEST_GENERATOR}")
                message(FATAL_ERROR "Consumer lost the parent generator: ${generator}")
            endif()
        endif()
        if(NOT probe AND NOT "${output}${error}" MATCHES "parent_app_native will open the default browser instead")
            message(FATAL_ERROR "Consumer fallback must explain the default browser: ${output}${error}")
        endif()
        if(NOT probe)
            # parent, sibling and their native helper targets share one configuration: one advice only.
            string(REGEX MATCHALL "no native engine will be built" advice "${output}${error}")
            list(LENGTH advice advice_count)
            if(NOT advice_count EQUAL 1)
                message(FATAL_ERROR "Fallback advice must be printed once, got ${advice_count}: ${output}${error}")
            endif()
        endif()
    endforeach()
endforeach()

# Explicit OFF is distinct from a probe that cannot find the dependency.
message(STATUS "Consumer configure: explicit OFF")
execute_process(COMMAND "${CMAKE_COMMAND}" --fresh ${configure_args}
    -S "${ROOT}/test/cmake/frontend-consumer" -B "${WORK_DIR}/explicit-off"
    "-DROOT=${ROOT}" -DPROBE=OFF -DFULL_PROJECT=ON "-DCPM_MODULE=${CPM_MODULE}"
    -DWEBFRONT_EMBED_CONSUMER=OFF ${DEPENDENCY_ARGS}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT "${output}${error}" MATCHES "explicitly disabled")
    message(FATAL_ERROR "Explicit OFF must be identified: ${output}${error}")
endif()

# A support module's final option is what other directories observe.
message(STATUS "Consumer configure: support option change")
execute_process(COMMAND "${CMAKE_COMMAND}" --fresh ${configure_args}
    -S "${ROOT}/test/cmake/frontend-consumer" -B "${WORK_DIR}/support-change"
    "-DROOT=${ROOT}" -DPROBE=ON -DMUTATE_OTHER=ON
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Support module option change: ${output}${error}")
endif()

# A consumer opts in to one of WebFront's test options only by its prefixed name (#412).
message(STATUS "Consumer configure: prefixed coverage option")
execute_process(COMMAND "${CMAKE_COMMAND}" --fresh ${configure_args}
    -S "${ROOT}/test/cmake/frontend-consumer" -B "${WORK_DIR}/consumer-coverage"
    "-DROOT=${ROOT}" -DPROBE=ON -DFULL_PROJECT=ON "-DCPM_MODULE=${CPM_MODULE}"
    -DWEBFRONT_ENABLE_COVERAGE=ON ${DEPENDENCY_ARGS}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer WEBFRONT_ENABLE_COVERAGE: ${output}${error}")
endif()
