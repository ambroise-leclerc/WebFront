# Global guard of #303: no path of WebFront depends on Node.js, npm or npx. Every file of the project is audited, not only the
# CMake find_program calls: sources, scripts, manifests, locks, workflows, development configuration, current documentation and
# the generated files that are allowed. The registries, formats and metadata of npm (package.json, integrities, semver, the
# resolution fields) are not matched: only imports and APIs of Node, commands of node/npm/npx, their configuration, their
# requirement in current documentation and the "like node.js" positioning of #303 are.
#
# The files are the tracked ones (`git ls-files`); outside a work tree (a source archive) the equivalent inventory is every file
# of the tree except the untracked build products (build*/, _deps/, .cache/). The exceptions are a list of files, each with the
# class of match it allows and why (test/cmake/NodeGuardExceptions.cmake); an exception that matches nothing fails too.
#   cmake -DROOT=<checkout> [-DGIT_EXECUTABLE=<git>] [-DEXCEPTIONS=<file>] -P test/cmake/CheckNoNode.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()
if(NOT DEFINED EXCEPTIONS)
    set(EXCEPTIONS "${CMAKE_CURRENT_LIST_DIR}/NodeGuardExceptions.cmake")
endif()

# <class>|<regular expression>: a CMake regular expression has no \b or lookaround, so a word is delimited by a character class.
set(word_start "(^|[^A-Za-z0-9_./@-])")
set(patterns
    "node-import|[\"'`]node:[a-z_]"
    "commonjs|${word_start}require\\([\"']"
    "node-api|process\\.(argv|exit|execPath|platform|exitCode|cwd|stdout|stderr|versions|env)"
    "node-command|${word_start}(node|nodejs)[ \t]+(-[-A-Za-z]|[^ \t\n]*\\.(mjs|cjs|js)([^A-Za-z]|$))"
    "npm-command|${word_start}npm[ \t]+((ci|install|i|run|test|exec|start|publish|init|audit|update)([^A-Za-z0-9_]|$)|-)"
    "tool-word|${word_start}(npx|nodejs|corepack)([^A-Za-z0-9_-]|$)"
    "shebang|#![^\n]*node"
    "node-config|setup-node|node_modules|NODE_EXECUTABLE|REQUIRE_NODE|find_program\\([^)]*node|\"engines\""
    "requirement|(require|requires|required|need|needs|needed|use|uses) (Node|npm)([^A-Za-z]|$)"
    "positioning|à la [Nn]ode|like Node\\.js|[Nn]ode\\.js-like|npm/npx|npx/node|\\| *(npm|npx) *\\|")
set(binary_extensions ico png jpg jpeg gif wasm tgz gz zip woff woff2 ttf otf)

# --- the inventory
set(files "")
if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c core.quotepath=off ls-files WORKING_DIRECTORY "${ROOT}" OUTPUT_VARIABLE listed
        RESULT_VARIABLE git_status ERROR_QUIET)
endif()
if(GIT_EXECUTABLE AND git_status EQUAL 0 AND listed)
    string(REPLACE "\n" ";" files "${listed}")
else()
    file(GLOB_RECURSE absolute RELATIVE "${ROOT}" "${ROOT}/*" "${ROOT}/.*/*")
    foreach(file IN LISTS absolute)
        if(NOT file MATCHES "^(build[^/]*|_deps|\\.cache|\\.git)/")
            list(APPEND files "${file}")
        endif()
    endforeach()
endif()
list(SORT files)
list(LENGTH files file_count)
if(file_count LESS 1)
    message(FATAL_ERROR "The inventory of ${ROOT} is empty")
endif()

# --- the exceptions: set(NODE_GUARD_EXCEPTIONS "<file>|<class>|<reason>" ...)
set(NODE_GUARD_EXCEPTIONS "")
include("${EXCEPTIONS}")
set(allowed "")
foreach(exception IN LISTS NODE_GUARD_EXCEPTIONS)
    string(REPLACE "|" ";" parts "${exception}")
    list(LENGTH parts parts_count)
    if(NOT parts_count EQUAL 3)
        message(FATAL_ERROR "An exception is <file>|<class>|<reason>: ${exception}")
    endif()
    list(GET parts 0 file)
    list(GET parts 1 class)
    list(GET parts 2 reason)
    if(NOT reason)
        message(FATAL_ERROR "The exception for ${file} (${class}) has no reason")
    endif()
    list(APPEND allowed "${file}:${class}")
endforeach()

# --- the audit
set(violations "")
set(used "")
foreach(file IN LISTS files)
    get_filename_component(extension "${file}" LAST_EXT)
    string(REPLACE "." "" extension "${extension}")
    string(TOLOWER "${extension}" extension)
    if(extension IN_LIST binary_extensions OR NOT EXISTS "${ROOT}/${file}")
        continue()
    endif()
    file(READ "${ROOT}/${file}" content)
    # The compile-time constant of the browser bundlers (React reads it) is not an API of Node.
    string(REPLACE "process.env.NODE_ENV" "" content "${content}")
    foreach(entry IN LISTS patterns)
        string(FIND "${entry}" "|" bar)
        string(SUBSTRING "${entry}" 0 ${bar} class)
        math(EXPR start "${bar} + 1")
        string(SUBSTRING "${entry}" ${start} -1 expression)
        if(content MATCHES "${expression}")
            set(matched "${CMAKE_MATCH_0}")
            if("${file}:${class}" IN_LIST allowed)
                list(APPEND used "${file}:${class}")
            else()
                # The line of the first match, for the report.
                string(FIND "${content}" "${matched}" offset)
                string(SUBSTRING "${content}" 0 ${offset} before)
                string(REGEX MATCHALL "\n" breaks "${before}")
                list(LENGTH breaks line)
                math(EXPR line "${line} + 1")
                string(REPLACE "\n" " " matched "${matched}")
                list(APPEND violations "${file}:${line}: [${class}] ${matched}")
            endif()
        endif()
    endforeach()
endforeach()

set(unused "")
foreach(entry IN LISTS allowed)
    if(NOT entry IN_LIST used)
        list(APPEND unused "${entry}")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n  " text)
    message(FATAL_ERROR "Node.js, npm or npx appear outside the exceptions of the guard:\n  ${text}")
endif()
if(unused)
    list(JOIN unused "\n  " text)
    message(FATAL_ERROR "These exceptions of the guard match nothing any more; remove them:\n  ${text}")
endif()
message(STATUS "${file_count} files audited: no dependency on Node.js, npm or npx")
