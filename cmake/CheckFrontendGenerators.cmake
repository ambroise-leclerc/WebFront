# Check that fixture-owned build trees are recreated for each requested generator. The parent
# location persists, but CheckFrontendSelection removes each case's cache before configuring.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT)
    message(FATAL_ERROR "ROOT must point at the WebFront checkout")
endif()
foreach(generator Ninja "Ninja Multi-Config" Ninja)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "CMAKE_GENERATOR=${generator}"
            "${CMAKE_COMMAND}" -DROOT=${ROOT} -P "${ROOT}/cmake/CheckFrontendSelection.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Frontend selection with ${generator} failed: ${output}${error}")
    endif()
    file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/frontend-tests/extension/CMakeCache.txt"
        configured_generator REGEX "^CMAKE_GENERATOR:INTERNAL=")
    if(NOT configured_generator STREQUAL "CMAKE_GENERATOR:INTERNAL=${generator}")
        message(FATAL_ERROR "Expected generator '${generator}', got '${configured_generator}'")
    endif()
endforeach()
