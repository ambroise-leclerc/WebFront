# Registry of the frontends that the examples and the browser tests can select. Adding a frontend
# means adding one entry here (plus its support module); the selectors below derive their allowed
# values, defaults, diagnostics and C++ type names from it.
#
# Entry format: <Name>:<availability option>:<policy>
#   - the availability option enables the frontend (empty: always available, not embedded);
#   - policy "strict": for selectors that pass REQUIRE_AVAILABLE, selecting it while its option is OFF
#     is a configuration error, because the test lane can never run; "lenient": the build is still valid,
#     since a build without the option intentionally compiles it to check the diagnostic it throws (CEF).
#     Selectors without REQUIRE_AVAILABLE (the demo) accept any registered frontend; an unavailable one
#     throws a clear error when opened.
# Order is the default-selection priority among embedded frontends: native web views before CEF.
set(WEBFRONT_FRONTEND_REGISTRY
    "DefaultBrowser::lenient"
    "WebView2:WEBFRONT_EMBED_WEBVIEW2:strict"
    "CEF:WEBFRONT_EMBED_CEF:lenient")

# Sets <out> to the registered frontend names, optionally only the embedded ones.
function(webfront_frontend_names out)
    cmake_parse_arguments(ARG "EMBEDDED_ONLY" "" "" ${ARGN})
    set(names "")
    foreach(entry IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        string(REPLACE ":" ";" fields "${entry}")
        list(GET fields 0 name)
        list(GET fields 1 option)
        if(ARG_EMBEDDED_ONLY AND option STREQUAL "")
            continue()
        endif()
        list(APPEND names "${name}")
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# Sets <out> to the first embedded frontend whose option is ON, or to <FALLBACK> when none is.
function(webfront_default_embedded_frontend out)
    cmake_parse_arguments(ARG "" "FALLBACK" "" ${ARGN})
    foreach(entry IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        string(REPLACE ":" ";" fields "${entry}")
        list(GET fields 0 name)
        list(GET fields 1 option)
        if(NOT option STREQUAL "")
            if(${option})
                set(${out} "${name}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
    set(${out} "${ARG_FALLBACK}" PARENT_SCOPE)
endfunction()

# Declares the cache variable <var> (default <default>), restricts it to the registered frontends and
# with REQUIRE_AVAILABLE rejects a "strict" frontend whose availability option is OFF. Sets <var>_TYPE in the caller to the C++
# type name, e.g. webfront::frontend::CEFFrontend.
function(webfront_select_frontend var default description)
    cmake_parse_arguments(ARG "EMBEDDED_ONLY;REQUIRE_AVAILABLE" "" "" ${ARGN})
    set(scope "")
    if(ARG_EMBEDDED_ONLY)
        set(scope EMBEDDED_ONLY)
    endif()
    webfront_frontend_names(allowed ${scope})
    list(JOIN allowed ", " allowed_text)
    set(${var} "${default}" CACHE STRING "${description}: ${allowed_text}")
    set_property(CACHE ${var} PROPERTY STRINGS ${allowed})
    set(selected "${${var}}")
    if(NOT selected IN_LIST allowed)
        message(FATAL_ERROR "${var} must be one of: ${allowed_text} (got '${selected}')")
    endif()
    foreach(entry IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        string(REPLACE ":" ";" fields "${entry}")
        list(GET fields 0 name)
        list(GET fields 1 option)
        list(GET fields 2 policy)
        if(ARG_REQUIRE_AVAILABLE AND name STREQUAL selected AND policy STREQUAL "strict")
            if(NOT ${option})
                message(FATAL_ERROR "${var}=${selected} requires ${option}=ON")
            endif()
        endif()
    endforeach()
    set(${var}_TYPE "webfront::frontend::${selected}Frontend" PARENT_SCOPE)
endfunction()
