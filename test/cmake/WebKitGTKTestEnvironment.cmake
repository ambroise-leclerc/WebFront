include_guard(GLOBAL)

# Configure each explicitly registered test and preserve the common helper's environment.
function(webfront_configure_webkitgtk_test name)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-data"
        "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-cache"
        "GDK_BACKEND=x11" "GSK_RENDERER=cairo" "LIBGL_ALWAYS_SOFTWARE=1")
    # XTest controls the display's global pointer. Parallel windows on the suite's shared display can cover the
    # file input or steal a press/release; the port lock does not protect against tests using ephemeral ports.
    # WithDisplay.sh then starts a private Xvfb even when RunTestSuite enabled display sharing for the suite.
    if(name MATCHES "^WebKitGTKAdapter-(choose-file|cancel-choose|close-with-dialog)$")
        set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT "WEBFRONT_TEST_SHARED_DISPLAY=0")
    endif()
endfunction()
