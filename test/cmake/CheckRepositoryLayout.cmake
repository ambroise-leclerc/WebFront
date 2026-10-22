# The tracked root of the repository is a closed list, so that the README stays visible on the
# GitHub page. third_party/ is a temporary exception removed by #319.
#   cmake -DROOT=<checkout> [-DGIT_EXECUTABLE=<git>] -P test/cmake/CheckRepositoryLayout.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(expected
    .clang-format .clang-tidy .devcontainer .gitattributes .github .gitignore
    AGENTS.md CMakeLists.txt README.md
    cmake docs examples include js test third_party)

set(actual "")
set(from_git OFF)
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE git)
endif()
if(GIT_EXECUTABLE AND EXISTS "${ROOT}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" ls-files
        WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE tracked RESULT_VARIABLE git_result ERROR_QUIET)
    if(git_result EQUAL 0)
        set(from_git ON)
        string(REPLACE "\n" ";" tracked "${tracked}")
        foreach(path IN LISTS tracked)
            string(REGEX REPLACE "/.*$" "" top "${path}")
            list(APPEND actual "${top}")
        endforeach()
    endif()
endif()
if(NOT from_git)
    # A source archive holds tracked content only, minus its export-ignore entries; a local
    # checkout without Git metadata may also hold build trees, which are not tracked content.
    file(GLOB entries LIST_DIRECTORIES true RELATIVE "${ROOT}" "${ROOT}/*" "${ROOT}/.[!.]*")
    list(FILTER entries EXCLUDE REGEX "^(build|\\.git$)")
    set(actual ${entries})
endif()
# .gitattributes and .gitignore are export-ignored, so archives legitimately lack them.
set(archive_optional .gitattributes .gitignore)
list(REMOVE_ITEM actual "" ${archive_optional})
list(REMOVE_ITEM expected ${archive_optional})
list(REMOVE_DUPLICATES actual)
list(SORT actual)
list(SORT expected)

if(NOT actual STREQUAL expected)
    set(unexpected ${actual})
    list(REMOVE_ITEM unexpected ${expected})
    set(missing ${expected})
    list(REMOVE_ITEM missing ${actual})
    message(FATAL_ERROR "The repository root drifted from the declared layout.\n"
        "  unexpected: ${unexpected}\n  missing: ${missing}")
endif()

# include/ exposes a single namespace directory, which holds headers only (no JavaScript, so that
# installers need no filter).
file(GLOB include_entries LIST_DIRECTORIES true RELATIVE "${ROOT}/include" "${ROOT}/include/*")
if(NOT include_entries STREQUAL "webfront")
    message(FATAL_ERROR "include/ must contain only webfront/, found: ${include_entries}")
endif()
file(GLOB_RECURSE include_files LIST_DIRECTORIES false RELATIVE "${ROOT}/include/webfront" "${ROOT}/include/webfront/*")
foreach(file IN LISTS include_files)
    if(NOT file MATCHES "\\.(hpp|h)$")
        message(FATAL_ERROR "include/webfront must hold headers only: ${file}")
    endif()
endforeach()
