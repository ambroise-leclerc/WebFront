# Exercise the actual CPM dependency-option scope with local sources: no compiler, SDK or network.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT EXISTS "${CPM_MODULE}")
    message(FATAL_ERROR "ROOT and the already-downloaded CPM_MODULE must be supplied")
endif()
foreach(state undefined cache-ON cache-OFF cache-empty normal-ON normal-OFF normal-empty
              normal-ON-cache-OFF normal-OFF-cache-ON)
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/cef-testing-state/${state}")
    file(REMOVE_RECURSE "${directory}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/cef-testing" -B "${directory}"
            -DROOT=${ROOT} -DCPM_MODULE=${CPM_MODULE} -DSTATE=${state}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "BUILD_TESTING state ${state}: ${output}${error}")
    endif()
endforeach()

# Apple's first configure must work with CPM sources outside _deps, without patching the SDK.
foreach(kind static interface missing)
    set(directory "${CMAKE_CURRENT_BINARY_DIR}/cef-testing-state/apple-${kind}")
    file(REMOVE_RECURSE "${directory}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/cef-testing" -B "${directory}"
            -DROOT=${ROOT} -DCPM_MODULE=${CPM_MODULE} -DSTATE=undefined
            -DSIMULATE_APPLE=ON -DWRAPPER_KIND=${kind}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(kind STREQUAL "static")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Apple static wrapper: ${output}${error}")
        endif()
    elseif(result EQUAL 0 OR NOT "${output}${error}" MATCHES "requires the real libcef_dll_wrapper static library")
        message(FATAL_ERROR "Apple ${kind} wrapper must fail clearly: ${output}${error}")
    endif()
endforeach()
