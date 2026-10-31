include("${ROOT}/cmake/Frontends.cmake")
# The fixture owns the native selection without changing the real target OS: the full
# project must still discover platform libraries (notably Windows threads) normally.
foreach(engine WebView2 WKWebView WebKitGTK)
    set_property(GLOBAL PROPERTY WEBFRONT_FRONTEND_${engine}_NATIVE OFF)
endforeach()
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
webfront_register_frontend(ConsumerNative
    NATIVE OPTION WEBFRONT_EMBED_CONSUMER POLICY strict DESCRIPTION "Consumer test engine"
    PLATFORMS "${CMAKE_SYSTEM_NAME}" SUPPORT ConsumerSupport SETUP consumer_setup
    TEST_PLATFORMS "${CMAKE_SYSTEM_NAME}" TEST_NAME ConsumerTest TEST_TIMEOUT 5)
webfront_register_frontend(ConsumerOther
    OPTION WEBFRONT_EMBED_CONSUMER_OTHER POLICY strict DESCRIPTION "Other embedded test engine"
    PLATFORMS "${CMAKE_SYSTEM_NAME}" SUPPORT ConsumerSupport SETUP consumer_other_setup
    TEST_PLATFORMS "${CMAKE_SYSTEM_NAME}" TEST_NAME ConsumerOtherTest TEST_TIMEOUT 5)
# Keep the fixture independent of the host's SDKs and all dependency downloads.
set(WEBFRONT_EMBED_WEBVIEW2 OFF)
set(WEBFRONT_EMBED_WKWEBVIEW OFF)
set(WEBFRONT_EMBED_WEBKITGTK OFF)
set(WEBFRONT_EMBED_CONSUMER_OTHER ON)
function(webfront_probe_native_frontend name out reason)
    set(${out} "${PROBE}" PARENT_SCOPE)
    set(${reason} "controlled consumer probe" PARENT_SCOPE)
endfunction()
if(FULL_PROJECT)
    # Reuse the already provisioned CPM module too, not only dependency source trees.
    # This configure must stay offline even when the outer build has no shared CPM cache.
    if(CPM_MODULE)
        set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_BINARY_DIR}/cached-inputs")
        file(MAKE_DIRECTORY "${CPM_SOURCE_CACHE}/cpm")
        get_filename_component(cpm_filename "${CPM_MODULE}" NAME)
        file(COPY_FILE "${CPM_MODULE}" "${CPM_SOURCE_CACHE}/cpm/${cpm_filename}" ONLY_IF_DIFFERENT)
    endif()
    set(ENABLE_TESTING OFF)
    set(ENABLE_FUZZING OFF)
    set(WEBFRONT_BUILD_EXAMPLES OFF)
    add_subdirectory("${ROOT}" webfront)
    if(NOT WEBFRONT_AUTODETECT_NATIVE)
        message(FATAL_ERROR "Native detection must default to ON for consumers without examples")
    endif()
else()
    webfront_autodetect_native_frontends()
    webfront_declare_frontend_options()
    webfront_include_frontend_support()
endif()
