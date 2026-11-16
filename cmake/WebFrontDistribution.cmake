# Build policy and package integration for native WebKitGTK applications. No runtime is added.
include_guard(GLOBAL)

# webfront_use_distribution(target): select the distribution policy for this executable only.
function(webfront_use_distribution target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "webfront_use_distribution: unknown target '${target}'")
    endif()
    get_target_property(kind "${target}" TYPE)
    if(NOT kind STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "webfront_use_distribution requires an executable target")
    endif()
    target_compile_definitions("${target}" PRIVATE WEBFRONT_DISTRIBUTION=1)
    message(STATUS "WebFront ${target}: DISTRIBUTION policy (WebKitGTK sandbox required)")
endfunction()

# webfront_generate_webkitgtk_profile(TARGET app EXECUTABLE /usr/bin/app OUTPUT generated/profile)
# EXECUTABLE is the final installed path, not the build path. Packaging owns installation/reload (dh_apparmor).
# Generating the file never installs it or changes the host's AppArmor policy.
function(webfront_generate_webkitgtk_profile)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "TARGET;EXECUTABLE;OUTPUT" "")
    if(ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES OR NOT ARG_TARGET OR NOT ARG_EXECUTABLE OR NOT ARG_OUTPUT)
        message(FATAL_ERROR "webfront_generate_webkitgtk_profile requires TARGET, EXECUTABLE and OUTPUT")
    endif()
    if(NOT ARG_TARGET MATCHES "^[A-Za-z0-9_.-]+$")
        message(FATAL_ERROR "webfront_generate_webkitgtk_profile: target must have a literal profile name")
    endif()
    # AppArmor attachment patterns and substitutions must never widen this per-executable grant.
    if(NOT ARG_EXECUTABLE MATCHES "^/[A-Za-z0-9_./+ -]+$" OR ARG_EXECUTABLE MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "webfront_generate_webkitgtk_profile: EXECUTABLE must be an absolute literal installed path without patterns")
    endif()
    webfront_use_distribution("${ARG_TARGET}")
    string(SHA256 executable_hash "${ARG_EXECUTABLE}")
    string(SUBSTRING "${executable_hash}" 0 16 executable_hash)
    get_filename_component(output "${ARG_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(profile_file "${output}" NAME)
    if(NOT profile_file MATCHES "^[A-Za-z0-9_.-]+$")
        message(FATAL_ERROR "webfront_generate_webkitgtk_profile: OUTPUT must have a literal profile filename")
    endif()
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT
"# Installed with the application; enables WebKit's own sandbox, not application confinement.
abi <abi/4.0>,
include <tunables/global>
profile webfront-@ARG_TARGET@-@executable_hash@ \"@ARG_EXECUTABLE@\" flags=(unconfined) {
    userns,
    include if exists <local/@profile_file@>
}
" @ONLY)
endfunction()
