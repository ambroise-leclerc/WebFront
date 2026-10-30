# The module scan check warns about targets linking WebFront::WebFront that CMake would scan, and only
# about those.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT and WORK_DIR are required")
endif()

function(_configure scenario expect_warning)
    set(build "${WORK_DIR}/${scenario}")
    file(REMOVE_RECURSE "${build}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${ROOT}/test/cmake/module-scan" -B "${build}"
            "-DWEBFRONT_SOURCE_DIR=${ROOT}" -DSCENARIO=${scenario} "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${scenario}: configure failed\n${out}\n${err}")
    endif()
    # CMake wraps warning text, so compare with whitespace collapsed.
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${out}${err}")
    string(FIND "${text}" "affected targets linking WebFront::WebFront: probe" position)
    if(expect_warning AND position EQUAL -1)
        message(FATAL_ERROR "${scenario}: expected a module scan warning\n${out}\n${err}")
    elseif(NOT expect_warning AND NOT position EQUAL -1)
        message(FATAL_ERROR "${scenario}: unexpected module scan warning\n${out}\n${err}")
    endif()
endfunction()

_configure(default ON)
_configure(variable OFF)
_configure(function OFF)
_configure(unrelated OFF)
