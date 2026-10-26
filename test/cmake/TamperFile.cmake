# Appends a byte to a file, to simulate a published file that changed.
# Usage: cmake -DFILE=<path> -P TamperFile.cmake
if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "${FILE} does not exist")
endif()
file(APPEND "${FILE}" "x")
