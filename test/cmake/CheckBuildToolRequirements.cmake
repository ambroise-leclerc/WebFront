# Reject an LTS CMake before policy queries, project discovery or dependency downloads.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK)
    message(FATAL_ERROR "ROOT and WORK are required")
endif()
file(MAKE_DIRECTORY "${WORK}")
foreach(version 3.22.1 3.28.3)
    foreach(entry CMakeLists.txt cmake/DeveloperCompatibility.cmake)
        # Override only the reported version in an isolated script process. In particular,
        # a missing guard on the root must not be mistaken for its later project() error.
        file(WRITE "${WORK}/old-cmake.cmake"
            "set(CMAKE_VERSION ${version})\ninclude(\"${ROOT}/${entry}\")\n")
        execute_process(COMMAND "${CMAKE_COMMAND}" -P "${WORK}/old-cmake.cmake"
            RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
        string(CONCAT diagnostic "${output}" "${error}")
        if(status EQUAL 0 OR NOT diagnostic MATCHES "WebFront requires CMake 3.31 or newer"
                OR NOT diagnostic MATCHES "found ${version}"
                OR NOT diagnostic MATCHES "README.md#build-tools-on-lts-distributions")
            message(FATAL_ERROR "${entry} did not explain how to replace CMake ${version}:\n${diagnostic}")
        endif()
    endforeach()
endforeach()
