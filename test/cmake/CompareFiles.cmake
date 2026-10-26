# Fails unless every listed file is byte for byte the same in two directories.
# Usage: cmake -DLEFT=<dir> -DRIGHT=<dir> "-DFILES=<name>;<name>..." -P CompareFiles.cmake
foreach(name IN LISTS FILES)
    foreach(side LEFT RIGHT)
        if(NOT EXISTS "${${side}}/${name}")
            message(FATAL_ERROR "${${side}}/${name} does not exist")
        endif()
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${LEFT}/${name}" "${RIGHT}/${name}" RESULT_VARIABLE different)
    if(different)
        message(FATAL_ERROR "${name} differs between ${LEFT} and ${RIGHT}")
    endif()
    message(STATUS "same ${name}")
endforeach()
