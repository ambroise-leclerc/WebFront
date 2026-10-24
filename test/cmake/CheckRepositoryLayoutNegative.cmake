# The layout guard must accept the declared root and reject a vendored third-party source, wherever it is tracked.
#   cmake -DROOT=<checkout> -DGIT_EXECUTABLE=<git> -DWORK_DIR=<scratch> -P test/cmake/CheckRepositoryLayoutNegative.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT OR NOT GIT_EXECUTABLE OR NOT WORK_DIR)
    message(FATAL_ERROR "ROOT, GIT_EXECUTABLE and WORK_DIR are required")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/include/webfront")
foreach(entry .clang-format .clang-tidy .devcontainer .gitattributes .github .gitignore AGENTS.md CMakeLists.txt README.md
        webfront-packages.lock.json webfront.project.json cmake docs examples js test)
    file(WRITE "${WORK_DIR}/${entry}/placeholder" "")
endforeach()
file(WRITE "${WORK_DIR}/include/webfront/WebFront.hpp" "")

function(git)
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN} WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${error}")
    endif()
endfunction()

function(run_guard result_variable output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DROOT=${WORK_DIR} -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -P "${CMAKE_CURRENT_LIST_DIR}/CheckRepositoryLayout.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    set(${result_variable} "${result}" PARENT_SCOPE)
    set(${output_variable} "${output}${error}" PARENT_SCOPE)
endfunction()

git(init -q)
git(add -A -f)
run_guard(clean_result clean_output)
if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR "The guard rejects the declared layout:\n${clean_output}")
endif()

foreach(rogue js/developer/vendor/lib.mjs js/third_party/lib.mjs js/node_modules/lib/index.js)
    file(WRITE "${WORK_DIR}/${rogue}" "export {};\n")
    git(add -f "${rogue}")
    run_guard(rogue_result rogue_output)
    if(rogue_result EQUAL 0 OR NOT rogue_output MATCHES "not vendored")
        message(FATAL_ERROR "The guard accepts the vendored source ${rogue}:\n${rogue_output}")
    endif()
    git(rm -q -f --cached "${rogue}")
endforeach()

# A root directory outside the declared list is refused too.
file(WRITE "${WORK_DIR}/extras/notes.txt" "notes\n")
git(add -f extras/notes.txt)
run_guard(root_result root_output)
if(root_result EQUAL 0 OR NOT root_output MATCHES "unexpected: extras")
    message(FATAL_ERROR "The guard accepts an undeclared root directory:\n${root_output}")
endif()
