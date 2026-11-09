# Regression for CheckReleaseWorkflow.cmake: the workflow is accepted, and each deviation from the table of ADR-021 fails.
cmake_minimum_required(VERSION 3.31)
if(NOT ROOT OR NOT WORK)
    message(FATAL_ERROR "ROOT and WORK are required")
endif()
set(workflow "${ROOT}/.github/workflows/ReleaseDeveloper.yml")
file(READ "${workflow}" original)
file(MAKE_DIRECTORY "${WORK}")

function(check description expected message)
    # Each mutation is a "from" then a "to" text (the first occurrence of "from" is replaced).
    set(content "${original}")
    set(arguments ${ARGN})
    while(arguments)
        list(POP_FRONT arguments from to)
        string(FIND "${content}" "${from}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${description}: the mutation target '${from}' is not in the workflow")
        endif()
        string(REPLACE "${from}" "${to}" content "${content}")
    endwhile()
    file(WRITE "${WORK}/ReleaseDeveloper.yml" "${content}")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DWORKFLOW=${WORK}/ReleaseDeveloper.yml"
        -P "${ROOT}/test/cmake/CheckReleaseWorkflow.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected STREQUAL "accept" AND NOT result EQUAL 0)
        message(FATAL_ERROR "${description}: refused: ${output}${error}")
    elseif(expected STREQUAL "refuse" AND result EQUAL 0)
        message(FATAL_ERROR "${description}: accepted")
    endif()
    if(NOT "${output}${error}" MATCHES "${message}")
        message(FATAL_ERROR "${description}: missing diagnostic '${message}': ${output}${error}")
    endif()
endfunction()

check("the workflow as written" accept "match the table")
check("a workflow-wide permission" refuse "top level" "permissions: {}\n" "permissions:\n  contents: write\n")
check("a verification that may write" refuse "Job verify"
    "    needs: [prepare, publish]\n    runs-on: ubuntu-26.04\n    timeout-minutes: 30\n    permissions:\n      contents: read\n"
    "    needs: [prepare, publish]\n    runs-on: ubuntu-26.04\n    timeout-minutes: 30\n    permissions:\n      contents: write\n")
check("a pull-request job that may write pull requests" refuse "Job pull-request"
    "    needs: [prepare, verify]\n    runs-on: ubuntu-26.04\n    timeout-minutes: 5\n    environment: developer-release-app\n    permissions:\n      contents: read\n"
    "    needs: [prepare, verify]\n    runs-on: ubuntu-26.04\n    timeout-minutes: 5\n    environment: developer-release-app\n    permissions:\n      contents: read\n      pull-requests: write\n")
check("a publication without pages permission" refuse "Job publish" "      pages: write\n" "")
check("a job without permissions of its own" refuse "Job tag declares no permissions"
    "    timeout-minutes: 5\n    permissions:\n      contents: write\n    steps:\n      - name: Tag" "    timeout-minutes: 5\n    steps:\n      - name: Tag")
check("an assistant attribution" refuse "attribution" "Pins the published" "Generated with a tool. Pins the published")
check("a preparation that rereads develop" refuse "Only wf-package"
    "needs.wf-package.outputs.source }}" "needs.wf-package.outputs.source }}\n          x: y\n      - uses: actions/checkout@v7\n        with:\n          ref: develop")
check("a rehearsal without the browser suite" refuse "rehearsal does not"
    "ctest --test-dir build-driver -L web-integration" "ctest --test-dir build-driver -R Nothing")
