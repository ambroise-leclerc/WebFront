# Public helper for a project that embeds WebFront with add_subdirectory() or FetchContent.
include_guard(GLOBAL)

# webfront_add_agent([INSTALL] [DESTINATION dir])
#
# The optional `webfront-agent` launcher is built by WebFront itself, from the same sources and with the engine the
# parent chose, when WEBFRONT_BUILD_AUTOMATION is ON and an embedded frontend is enabled. Set both before WebFront is
# added:
#
#   set(WEBFRONT_BUILD_AUTOMATION ON)
#   set(WEBFRONT_EMBED_WEBVIEW2 ON)          # or WKWEBVIEW, CEF, WEBKITGTK
#   FetchContent_MakeAvailable(WebFront)
#   webfront_add_agent(INSTALL)
#
# This function then checks that the target exists, with an explicit error otherwise (no engine is ever chosen
# silently), and with INSTALL installs the executable (default DESTINATION: bin). An installed CEF launcher still needs
# the engine's runtime files beside it, which this helper does not install. The agent drives an application that
# enables automation itself (webfront::automation::enableAutomation, docs/AUTOMATION.md); it grants nothing alone.
function(webfront_add_agent)
    cmake_parse_arguments(ARG "INSTALL" "DESTINATION" "" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "webfront_add_agent: invalid arguments (unknown: ${ARG_UNPARSED_ARGUMENTS}; keywords without a value: ${ARG_KEYWORDS_MISSING_VALUES})")
    endif()
    if(NOT TARGET webfront-agent)
        message(FATAL_ERROR
            "webfront_add_agent: the webfront-agent target does not exist. Set WEBFRONT_BUILD_AUTOMATION=ON and enable "
            "one embedded frontend (WEBFRONT_EMBED_WEBVIEW2, WEBFRONT_EMBED_WKWEBVIEW, WEBFRONT_EMBED_CEF or "
            "WEBFRONT_EMBED_WEBKITGTK) before adding WebFront.")
    endif()
    if(ARG_INSTALL)
        if(NOT ARG_DESTINATION)
            set(ARG_DESTINATION bin)
        endif()
        install(TARGETS webfront-agent RUNTIME DESTINATION "${ARG_DESTINATION}")
    endif()
endfunction()
