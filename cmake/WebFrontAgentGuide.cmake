# Public helpers that tell an agent, from the first configuration, how a WebFront application is developed.
include_guard(GLOBAL)

# The paths are computed inside the functions, from CMAKE_CURRENT_FUNCTION_LIST_DIR: a variable set here would belong to the
# scope that included this file, and the functions are called from its parent (add_subdirectory, FetchContent).

# webfront_write_agent_guide([DIRECTORY dir] [NAME file])
#
# Writes the agent guide of a WebFront application (the same text `wf create` puts beside a starter's sources) as
# AGENTS.md at the root of the calling project: the model (C++ with a TypeScript/TSX interface in ui/, developed with
# `wf dev`) and how an agent runs wf itself in JSON or MCP. Coding agents read that file before anything else.
#
#   add_subdirectory(WebFront)              # or FetchContent / CPM
#   webfront_write_agent_guide()            # <project>/AGENTS.md, once
#
# An existing file is never replaced or appended to, whatever wrote it: the function then says in one line where the
# guide is, so that a project with its own AGENTS.md can refer to it. DIRECTORY defaults to the top-level source
# directory, NAME to AGENTS.md (CLAUDE.md, for instance, for a client that reads that name).
function(webfront_write_agent_guide)
    cmake_parse_arguments(ARG "" "DIRECTORY;NAME" "" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "webfront_write_agent_guide: invalid arguments (unknown: ${ARG_UNPARSED_ARGUMENTS}; keywords without a value: ${ARG_KEYWORDS_MISSING_VALUES})")
    endif()
    if(NOT ARG_DIRECTORY)
        set(ARG_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
    if(NOT ARG_NAME)
        set(ARG_NAME AGENTS.md)
    endif()
    if(NOT ARG_NAME MATCHES "^[A-Za-z0-9._-]+$" OR ARG_NAME MATCHES "^[.]+$")
        message(FATAL_ERROR "webfront_write_agent_guide: NAME must be a plain file name (got '${ARG_NAME}')")
    endif()
    cmake_path(ABSOLUTE_PATH ARG_DIRECTORY BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    if(NOT IS_DIRECTORY "${ARG_DIRECTORY}")
        message(FATAL_ERROR "webfront_write_agent_guide: ${ARG_DIRECTORY} is not a directory")
    endif()
    cmake_path(NORMAL_PATH CMAKE_CURRENT_FUNCTION_LIST_DIR OUTPUT_VARIABLE here)
    set(source "${here}/../js/developer/starters/agent-guide.md")
    cmake_path(NORMAL_PATH source)
    set(destination "${ARG_DIRECTORY}/${ARG_NAME}")
    # EXISTS follows links: a dangling link is a file somebody placed there too.
    if(EXISTS "${destination}" OR IS_SYMLINK "${destination}")
        message(STATUS "WebFront: ${destination} exists and is left unchanged; the agent guide is ${source}")
        return()
    endif()
    file(COPY_FILE "${source}" "${destination}")
    message(STATUS "WebFront: wrote the agent guide ${destination}")
endfunction()

# One notice per configuration, for whoever reads the configure output first: the development model, the guide, and
# either how to attach to the launcher that is built or the option that builds it.
function(webfront_announce_agent_support)
    get_property(announced GLOBAL PROPERTY WEBFRONT_AGENT_SUPPORT_ANNOUNCED)
    if(announced)
        return()
    endif()
    set_property(GLOBAL PROPERTY WEBFRONT_AGENT_SUPPORT_ANNOUNCED ON)
    set(guide "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../docs/DEVELOPING_WITH_AGENTS.md")
    cmake_path(NORMAL_PATH guide)
    if(TARGET webfront-agent)
        set(launcher "the webfront-agent target runs wf for an agent in JSON or MCP (webfront-agent --agent-help)")
    else()
        set(launcher "webfront-agent, which runs wf for an agent in JSON or MCP, is not built: "
            "configure with -DWEBFRONT_BUILD_AUTOMATION=ON")
        string(JOIN "" launcher ${launcher})
    endif()
    message(STATUS "WebFront for agents: an application is C++ with a TypeScript/TSX interface in ui/, developed with `wf dev` "
        "(not pages served from documentRoot); ${launcher}. Guide: ${guide}"
        "; webfront_write_agent_guide() writes it as AGENTS.md in your project.")
endfunction()
