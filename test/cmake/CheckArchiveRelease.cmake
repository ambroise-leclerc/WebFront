# archive-release.sh must refuse a missing or invalid manifest before it calls GitHub.
# Usage: cmake -DSCRIPT=<archive-release.sh> -DWORK_DIR=<dir> -DBASH=<bash> -P CheckArchiveRelease.cmake
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/bin" "${WORK_DIR}/missing" "${WORK_DIR}/invalid")
file(WRITE "${WORK_DIR}/bin/gh" "#!/bin/sh\necho \"$*\" >> \"${WORK_DIR}/gh.log\"\nexit 1\n")
file(CHMOD "${WORK_DIR}/bin/gh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE "${WORK_DIR}/invalid/provision.json" "{ not json")
get_filename_component(tools "${SCRIPT}" DIRECTORY)
file(READ "${tools}/distribution.json" distribution)
string(JSON version GET "${distribution}" version)
foreach(case missing invalid)
    file(REMOVE "${WORK_DIR}/gh.log")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PATH=${WORK_DIR}/bin:$ENV{PATH}" GITHUB_REPOSITORY=o/r
            "${BASH}" "${SCRIPT}" developer-v${version} "${WORK_DIR}/${case}"
        RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
    if(status EQUAL 0)
        message(FATAL_ERROR "archive-release.sh accepted a ${case} manifest")
    endif()
    if(EXISTS "${WORK_DIR}/gh.log")
        file(READ "${WORK_DIR}/gh.log" calls)
        message(FATAL_ERROR "archive-release.sh called gh with a ${case} manifest: ${calls}")
    endif()
endforeach()
