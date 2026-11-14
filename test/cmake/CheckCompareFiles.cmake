# CompareFiles.cmake compares every listed file, and refuses a list that its caller split into separate arguments, of which
# CMake would keep only the first name: the reproducibility proof then compared a single file.
# Usage: cmake -DSOURCE_DIR=<checkout> -DWORK=<scratch> -P CheckCompareFiles.cmake
cmake_minimum_required(VERSION 3.31)
file(REMOVE_RECURSE "${WORK}")
foreach(side left right)
    file(WRITE "${WORK}/${side}/a.txt" "same")
    file(WRITE "${WORK}/${side}/b.txt" "same")
endforeach()
set(compare "${CMAKE_COMMAND}" -DLEFT=${WORK}/left -DRIGHT=${WORK}/right)
set(script -P "${SOURCE_DIR}/test/cmake/CompareFiles.cmake")

# The FILES argument is written in each call: passed through a function, the list would split.
function(expect case passes pattern result output)
    if(passes AND NOT result EQUAL 0 OR NOT passes AND result EQUAL 0 OR NOT output MATCHES "${pattern}")
        message(FATAL_ERROR "CompareFiles, ${case}: status ${result}, output:\n${output}")
    endif()
endfunction()

execute_process(COMMAND ${compare} "-DFILES=a.txt;b.txt" ${script} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
expect("identical files" TRUE "same a\\.txt.*same b\\.txt" "${result}" "${output}")
execute_process(COMMAND ${compare} -DFILES=a.txt b.txt ${script} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
expect("a split list" FALSE "Unexpected argument 'b\\.txt'" "${result}" "${output}")
execute_process(COMMAND ${compare} -DFILES= ${script} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
expect("no file" FALSE "FILES names no file" "${result}" "${output}")
file(WRITE "${WORK}/right/b.txt" "different")
execute_process(COMMAND ${compare} "-DFILES=a.txt;b.txt" ${script} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
expect("a differing file" FALSE "b\\.txt differs" "${result}" "${output}")
