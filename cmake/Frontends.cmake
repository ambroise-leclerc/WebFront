include_guard(GLOBAL)

# Include this internal module first from WebFront's root CMakeLists.txt. Child directories inherit
# its registry variables; include_guard(GLOBAL) does not share them with unrelated or parent scopes.
# Standalone inclusion from such a scope is not supported.

# Registration order is the priority for the default embedded frontend. A new engine needs one
# entry here and its support module. Options are always OFF by default and platform-checked before
# dependencies are fetched. POLICY lenient preserves the CEF-off diagnostic build of webtest.
function(webfront_register_frontend name)
    if(NOT "${name}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Frontend name '${name}' must be a C++ identifier")
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "XVFB_LINUX;ALWAYS_AVAILABLE;NATIVE" "OPTION;POLICY;DESCRIPTION;SUPPORT;SETUP;TEST_SETUP;TEST_NAME;TEST_TIMEOUT" "PLATFORMS;TEST_PLATFORMS")
    if(name IN_LIST WEBFRONT_FRONTEND_REGISTRY)
        message(FATAL_ERROR "Frontend '${name}' is already registered")
    endif()
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Frontend '${name}' has unknown arguments or missing values: ${ARG_UNPARSED_ARGUMENTS};${ARG_KEYWORDS_MISSING_VALUES}")
    endif()
    if(ARG_ALWAYS_AVAILABLE AND NOT DEFINED ARG_POLICY)
        set(ARG_POLICY lenient)
    endif()
    if(NOT ARG_POLICY MATCHES "^(strict|lenient)$")
        message(FATAL_ERROR "Frontend '${name}' requires POLICY strict or lenient")
    endif()
    if(ARG_OPTION)
        if(ARG_ALWAYS_AVAILABLE)
            message(FATAL_ERROR "Frontend '${name}' cannot combine OPTION and ALWAYS_AVAILABLE")
        endif()
        if(NOT ARG_OPTION MATCHES "^WEBFRONT_EMBED_[A-Z0-9_]+$")
            message(FATAL_ERROR "Frontend '${name}' requires a WEBFRONT_EMBED_<NAME> option")
        endif()
        foreach(field DESCRIPTION PLATFORMS SUPPORT TEST_PLATFORMS TEST_NAME TEST_TIMEOUT)
            if(NOT DEFINED ARG_${field} OR "${ARG_${field}}" STREQUAL "")
                message(FATAL_ERROR "Frontend '${name}' requires ${field}")
            endif()
        endforeach()
        if(NOT ARG_TEST_TIMEOUT MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "Frontend '${name}' requires a positive integer TEST_TIMEOUT")
        endif()
        # Resolve the module now, without loading platform SDKs for disabled engines. These
        # project files belong to the host checkout even when target includes use a sysroot.
        # Prefer WebFront-owned modules over equally named modules from a parent project.
        set(support_file support_file-NOTFOUND)
        find_file(support_file "${ARG_SUPPORT}.cmake"
            PATHS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" ${CMAKE_MODULE_PATH}
            NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
        if(NOT support_file)
            message(FATAL_ERROR "Frontend '${name}' SUPPORT module '${ARG_SUPPORT}' was not found")
        endif()
        set(ARG_SUPPORT_FILE "${support_file}")
        foreach(platform IN LISTS ARG_TEST_PLATFORMS)
            if(NOT platform IN_LIST ARG_PLATFORMS)
                message(FATAL_ERROR "Frontend '${name}' TEST_PLATFORMS contains unsupported '${platform}'")
            endif()
        endforeach()
    else()
        if(NOT ARG_ALWAYS_AVAILABLE)
            message(FATAL_ERROR "Frontend '${name}' requires OPTION or ALWAYS_AVAILABLE")
        endif()
        if(ARG_POLICY STREQUAL "strict")
            message(FATAL_ERROR "Frontend '${name}' cannot use POLICY strict with ALWAYS_AVAILABLE")
        endif()
        foreach(field DESCRIPTION PLATFORMS SUPPORT SETUP TEST_PLATFORMS TEST_SETUP TEST_NAME TEST_TIMEOUT)
            if(DEFINED ARG_${field})
                message(FATAL_ERROR "Frontend '${name}' cannot use ${field} without OPTION")
            endif()
        endforeach()
        if(ARG_XVFB_LINUX)
            message(FATAL_ERROR "Frontend '${name}' cannot use XVFB_LINUX without OPTION")
        endif()
        if(ARG_NATIVE)
            message(FATAL_ERROR "Frontend '${name}' cannot use NATIVE without OPTION")
        endif()
    endif()
    list(APPEND WEBFRONT_FRONTEND_REGISTRY "${name}")
    set(WEBFRONT_FRONTEND_REGISTRY "${WEBFRONT_FRONTEND_REGISTRY}" PARENT_SCOPE)
    foreach(field OPTION POLICY DESCRIPTION SUPPORT SUPPORT_FILE SETUP TEST_SETUP TEST_NAME TEST_TIMEOUT PLATFORMS TEST_PLATFORMS XVFB_LINUX NATIVE)
        set(WEBFRONT_FRONTEND_${name}_${field} "${ARG_${field}}" PARENT_SCOPE)
    endforeach()
endfunction()

webfront_register_frontend(DefaultBrowser ALWAYS_AVAILABLE)
webfront_register_frontend(WebView2
    NATIVE OPTION WEBFRONT_EMBED_WEBVIEW2 POLICY strict
    DESCRIPTION "Download the pinned WebView2 SDK and link its static loader (Windows only)"
    PLATFORMS Windows SUPPORT WebView2Support
    TEST_PLATFORMS Windows TEST_NAME WebFrontWebView2Integration TEST_TIMEOUT 90
    TEST_SETUP webfront_add_webview2_tests)
webfront_register_frontend(WKWebView
    NATIVE OPTION WEBFRONT_EMBED_WKWEBVIEW POLICY strict
    DESCRIPTION "Link the system Cocoa and WebKit frameworks for WKWebViewFrontend (macOS 13.3+ only)"
    PLATFORMS Darwin SUPPORT WKWebViewSupport
    TEST_PLATFORMS Darwin TEST_NAME WebFrontWKWebViewIntegration TEST_TIMEOUT 90
    TEST_SETUP webfront_add_wkwebview_tests)
webfront_register_frontend(WebKitGTK
    NATIVE OPTION WEBFRONT_EMBED_WEBKITGTK POLICY strict
    DESCRIPTION "Enable the system WebKitGTK 6.0 embedded frontend (Linux only)"
    PLATFORMS Linux SUPPORT WebKitGTKSupport
    TEST_PLATFORMS Linux TEST_NAME WebFrontWebKitGTKIntegration TEST_TIMEOUT 90 XVFB_LINUX
    TEST_SETUP webfront_add_webkitgtk_tests)
webfront_register_frontend(CEF
    OPTION WEBFRONT_EMBED_CEF POLICY lenient
    DESCRIPTION "Download and link CEF so CEFFrontend is available (Windows, Linux and macOS only). Other platforms fail at configure time"
    PLATFORMS Windows Linux Darwin SUPPORT CEFIntegration SETUP setup_cef_for_target
    TEST_PLATFORMS Linux TEST_NAME WebFrontBrowserIntegration TEST_TIMEOUT 60 XVFB_LINUX)

# Both option validation and standalone default selection use the registry's platform list.
function(webfront_frontend_supports_platform name out)
    set(supported OFF)
    if(CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${name}_PLATFORMS)
        set(supported ON)
    endif()
    set(${out} "${supported}" PARENT_SCOPE)
endfunction()

function(webfront_declare_frontend_options)
    foreach(name IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(option "${WEBFRONT_FRONTEND_${name}_OPTION}")
        if(option STREQUAL "")
            continue()
        endif()
        option(${option} "${WEBFRONT_FRONTEND_${name}_DESCRIPTION}" OFF)
        webfront_frontend_supports_platform("${name}" supported)
        if(${option} AND NOT supported)
            list(JOIN WEBFRONT_FRONTEND_${name}_PLATFORMS ", " platforms)
            message(FATAL_ERROR "${option} is supported only on: ${platforms} (got '${CMAKE_SYSTEM_NAME}')")
        endif()
    endforeach()
endfunction()


# Probes whether the native engine <name> can be built here. Sets <out> to ON or OFF and <reason> to a
# diagnostic for the OFF case. Only the registry's native engines are probed, never CEF. Kept
# separate so that configure-only tests can supply a deterministic answer.
function(webfront_probe_native_frontend name out reason)
    set(available OFF)
    set(why "")
    if(name STREQUAL "WebKitGTK")
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(WEBFRONT_AUTODETECT_WEBKITGTK QUIET webkitgtk-6.0)
        endif()
        if(WEBFRONT_AUTODETECT_WEBKITGTK_FOUND)
            set(available ON)
        else()
            set(why "pkg-config webkitgtk-6.0 was not found; install libwebkitgtk-6.0-dev (Ubuntu/Debian) or webkitgtk6.0-devel (Fedora), see docs/WEBKITGTK.md")
        endif()
    elseif(name STREQUAL "WKWebView")
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(why "WKWebViewFrontend requires Clang with Apple Blocks support")
        elseif(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.3")
            set(why "CMAKE_OSX_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET} is older than 13.3")
        elseif(NOT CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_HOST_SYSTEM_VERSION VERSION_LESS "22.4")
            set(why "macOS 13.3 or later is required (Darwin ${CMAKE_HOST_SYSTEM_VERSION})")
        else()
            set(available ON)
        endif()
    elseif(name STREQUAL "WebView2")
        if(MSVC)
            set(available ON)
        else()
            set(why "WebView2Frontend requires an MSVC-compatible compiler (MSVC or clang-cl)")
        endif()
    else()
        set(why "no probe is defined for ${name}")
    endif()
    set(${out} "${available}" PARENT_SCOPE)
    set(${reason} "${why}" PARENT_SCOPE)
endfunction()

# Enables, for this platform's native engine, an option the user has not set. Automatic values stay
# in normal variables: option() honors them (CMP0077), without creating a cache entry. A later
# configure probes again, while an explicit normal or cached value always wins, even when unchanged.
# A missing engine only produces a status message; the application reports it at run time.
function(webfront_autodetect_native_frontends)
    foreach(name IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(option "${WEBFRONT_FRONTEND_${name}_OPTION}")
        if(option STREQUAL "" OR NOT WEBFRONT_FRONTEND_${name}_NATIVE)
            continue()
        endif()
        webfront_frontend_supports_platform("${name}" supported)
        if(NOT supported)
            continue()
        endif()
        if(DEFINED ${option})
            continue()
        endif()
        webfront_probe_native_frontend("${name}" available reason)
        set(${option} "${available}" PARENT_SCOPE)
        if(available)
            message(STATUS "Native window: ${name} detected, ${option}=ON")
        else()
            message(STATUS "Native window: ${name} unavailable (${reason}), so applications using NativeApp will report it when started. Set -D${option}=ON once it is installed")
        endif()
    endforeach()
endfunction()

# Hook functions are supplied by the support module. Validate them immediately after loading it,
# rather than deferring an unknown-command error until target or test setup.
function(webfront_validate_frontend_hooks name)
    foreach(field SETUP TEST_SETUP)
        set(hook "${WEBFRONT_FRONTEND_${name}_${field}}")
        if(NOT hook STREQUAL "" AND NOT COMMAND "${hook}")
            message(FATAL_ERROR "Frontend '${name}' ${field} function '${hook}' is not defined by SUPPORT module '${WEBFRONT_FRONTEND_${name}_SUPPORT}'")
        endif()
    endforeach()
endfunction()

# A macro keeps dependency variables (CEF_ROOT, CPM source directories, etc.) in the caller's scope.
macro(webfront_include_frontend_support)
    foreach(_webfront_name IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(_webfront_option "${WEBFRONT_FRONTEND_${_webfront_name}_OPTION}")
        if(NOT _webfront_option STREQUAL "")
            if(${_webfront_option})
                include("${WEBFRONT_FRONTEND_${_webfront_name}_SUPPORT_FILE}")
                webfront_validate_frontend_hooks("${_webfront_name}")
            endif()
        endif()
    endforeach()
    unset(_webfront_name)
    unset(_webfront_option)
endmacro()

function(webfront_setup_frontends_for_target target)
    foreach(name IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(setup "${WEBFRONT_FRONTEND_${name}_SETUP}")
        if(NOT setup STREQUAL "")
            if(${WEBFRONT_FRONTEND_${name}_OPTION})
                cmake_language(CALL ${setup} ${target})
            endif()
        endif()
    endforeach()
endfunction()

# Separate discovery from registration so configure-only tests can supply a deterministic result.
function(webfront_find_xvfb out)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    if(XVFB_RUN_EXECUTABLE)
        set(${out} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../test/tools/WithDisplay.sh" PARENT_SCOPE)
    else()
        set(${out} "${XVFB_RUN_EXECUTABLE}" PARENT_SCOPE)
    endif()
endfunction()

function(webfront_set_browser_test_properties name assets_directory timeout)
    set_tests_properties(${name} PROPERTIES
        LABELS "web-integration" TIMEOUT "${timeout}"
        WORKING_DIRECTORY "${assets_directory}" RESOURCE_LOCK webfront_port_9002)
endfunction()

# Shared Jasmine and lifecycle registration; engine-specific cases stay in the support module.
# ARGUMENTS are passed to the Jasmine target (webtest runs `wf test` on a prepared project); SKIP_JASMINE registers the
# lifecycle and engine cases only, when the target cannot run on this configuration.
function(webfront_add_browser_tests target frontend assets_directory lifecycle_target)
    cmake_parse_arguments(ARG "SKIP_JASMINE" "" "ARGUMENTS" ${ARGN})
    if(NOT frontend IN_LIST WEBFRONT_FRONTEND_REGISTRY)
        message(FATAL_ERROR "Unknown browser-test frontend '${frontend}'")
    endif()
    set(option "${WEBFRONT_FRONTEND_${frontend}_OPTION}")
    if(option STREQUAL "")
        return()
    endif()
    if(NOT ${option})
        return()
    endif()
    if(NOT CMAKE_SYSTEM_NAME IN_LIST WEBFRONT_FRONTEND_${frontend}_TEST_PLATFORMS)
        list(JOIN WEBFRONT_FRONTEND_${frontend}_TEST_PLATFORMS ", " platforms)
        message(WARNING "${frontend} browser integration is not registered on ${CMAKE_SYSTEM_NAME}; validated test platforms: ${platforms}. Set WEBFRONT_WEBTEST_FRONTEND explicitly to a validated enabled engine.")
        return()
    endif()
    set(register_shared ON)
    set(command "$<TARGET_FILE:${target}>")
    # A frontend may test on several platforms; XVFB_LINUX only wraps its Linux runner.
    if(WEBFRONT_FRONTEND_${frontend}_XVFB_LINUX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        webfront_find_xvfb(XVFB_RUN_EXECUTABLE)
        if(NOT XVFB_RUN_EXECUTABLE)
            message(WARNING "xvfb-run was not found; the ${frontend} browser integration tests will not be registered")
            set(register_shared OFF)
        else()
            set(command "${XVFB_RUN_EXECUTABLE}" -a ${command})
        endif()
    endif()
    set(shared_tests "")
    if(register_shared)
        set(lifecycle_command "$<TARGET_FILE:${lifecycle_target}>")
        if(WEBFRONT_FRONTEND_${frontend}_XVFB_LINUX AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(lifecycle_command "${XVFB_RUN_EXECUTABLE}" -a ${lifecycle_command})
        endif()
        foreach(mode no-bridge early-close rejected-ready navigation-failure path-encoding content-policy navigation-allow-list)
            set(lifecycle_name "${frontend}Lifecycle-${mode}")
            add_test(NAME ${lifecycle_name} COMMAND ${lifecycle_command} ${mode})
            list(APPEND shared_tests "${lifecycle_name}")
            webfront_set_browser_test_properties("${lifecycle_name}" "${assets_directory}"
                "${WEBFRONT_FRONTEND_${frontend}_TEST_TIMEOUT}")
        endforeach()
        if(NOT ARG_SKIP_JASMINE)
            set(name "${WEBFRONT_FRONTEND_${frontend}_TEST_NAME}")
            add_test(NAME ${name} COMMAND ${command} ${ARG_ARGUMENTS})
            list(APPEND shared_tests "${name}")
            webfront_set_browser_test_properties("${name}" "${assets_directory}"
                "${WEBFRONT_FRONTEND_${frontend}_TEST_TIMEOUT}")
        endif()
    endif()
    # Per-engine tests may be independent of a display and must not be skipped with Jasmine.
    set(setup "${WEBFRONT_FRONTEND_${frontend}_TEST_SETUP}")
    if(NOT setup STREQUAL "")
        cmake_language(CALL ${setup} ${shared_tests})
    endif()
endfunction()

# Sets <out> to the registered frontend names, optionally only the embedded ones.
function(webfront_frontend_names out)
    cmake_parse_arguments(ARG "EMBEDDED_ONLY" "" "" ${ARGN})
    set(names "")
    foreach(entry IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(name "${entry}")
        set(option "${WEBFRONT_FRONTEND_${name}_OPTION}")
        if(ARG_EMBEDDED_ONLY AND option STREQUAL "")
            continue()
        endif()
        list(APPEND names "${name}")
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# Sets <out> to the first embedded frontend whose option is ON, or to <FALLBACK> when none is.
# Production configuration rejects enabled unsupported engines when declaring options. Keep this
# helper independently platform-aware for callers inspecting the registry, including test fixtures.
function(webfront_default_embedded_frontend out)
    cmake_parse_arguments(ARG "" "FALLBACK" "" ${ARGN})
    foreach(entry IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(name "${entry}")
        set(option "${WEBFRONT_FRONTEND_${name}_OPTION}")
        if(NOT option STREQUAL "")
            webfront_frontend_supports_platform("${name}" supported)
            if(${option} AND supported)
                set(${out} "${name}" PARENT_SCOPE)
                return()
            endif()
        endif()
    endforeach()
    set(${out} "${ARG_FALLBACK}" PARENT_SCOPE)
endfunction()

# Declares the cache variable <var> (default <default>), restricts it to the registered frontends and
# with REQUIRE_AVAILABLE rejects a "strict" frontend whose availability option is OFF. Sets <var>_TYPE in the caller to the C++
# type name, e.g. webfront::frontend::CEFFrontend. ALLOW_NATIVE adds the explicit platform alias.
function(webfront_select_frontend var default description)
    cmake_parse_arguments(ARG "EMBEDDED_ONLY;REQUIRE_AVAILABLE;ALLOW_NATIVE" "" "" ${ARGN})
    set(scope "")
    if(ARG_EMBEDDED_ONLY)
        set(scope EMBEDDED_ONLY)
    endif()
    webfront_frontend_names(allowed ${scope})
    if(ARG_ALLOW_NATIVE)
        list(APPEND allowed Native)
    endif()
    list(JOIN allowed ", " allowed_text)
    set(${var} "${default}" CACHE STRING "${description}: ${allowed_text}")
    set_property(CACHE ${var} PROPERTY STRINGS ${allowed})
    set(selected "${${var}}")
    if(NOT selected IN_LIST allowed)
        message(FATAL_ERROR "${var} must be one of: ${allowed_text} (got '${selected}')")
    endif()
    # Native is an explicit demo alias, never an engine registration or a default candidate.
    # Keep the underlying adapter's runtime diagnostic when its option is disabled.
    if(selected STREQUAL "Native")
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(native_option WEBFRONT_EMBED_WEBVIEW2)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(native_option WEBFRONT_EMBED_WKWEBVIEW)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(native_option WEBFRONT_EMBED_WEBKITGTK)
        else()
            message(FATAL_ERROR "${var}=Native supports only Windows, Darwin and Linux (got '${CMAKE_SYSTEM_NAME}')")
        endif()
        if(ARG_REQUIRE_AVAILABLE AND NOT ${native_option})
            message(FATAL_ERROR "${var}=Native requires ${native_option}=ON")
        endif()
        set(${var}_TYPE "webfront::frontend::NativeWebViewFrontend" PARENT_SCOPE)
        return()
    endif()
    # Enabled engines have already been platform-checked by webfront_declare_frontend_options().
    # An unavailable engine may still be selected for its runtime diagnostic (including CEF-off
    # webtest builds); selection alone therefore does not require a supported platform.
    foreach(entry IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        set(name "${entry}")
        set(option "${WEBFRONT_FRONTEND_${name}_OPTION}")
        set(policy "${WEBFRONT_FRONTEND_${name}_POLICY}")
        if(ARG_REQUIRE_AVAILABLE AND name STREQUAL selected AND policy STREQUAL "strict")
            if(NOT ${option})
                message(FATAL_ERROR "${var}=${selected} requires ${option}=ON")
            endif()
        endif()
    endforeach()
    set(${var}_TYPE "webfront::frontend::${selected}Frontend" PARENT_SCOPE)
endfunction()

# Sets <out> to the option of the native engine registered for this platform, or to "" when there is none.
function(webfront_native_frontend_option out)
    set(result "")
    foreach(name IN LISTS WEBFRONT_FRONTEND_REGISTRY)
        webfront_frontend_supports_platform("${name}" supported)
        if(WEBFRONT_FRONTEND_${name}_NATIVE AND supported)
            set(result "${WEBFRONT_FRONTEND_${name}_OPTION}")
            break()
        endif()
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()
