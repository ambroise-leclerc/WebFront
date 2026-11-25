include_guard(GLOBAL)

# The XDG roots a test replaces with its throwaway profile reach every program its application starts. Capturing them
# in a test property would freeze the configure-time environment; ctest instead includes cmake/CaptureRealXdg.cmake when
# it starts, which records the roots of the environment that launches the tests. Once per directory that has such tests.
function(webfront_export_real_xdg)
    get_property(registered DIRECTORY PROPERTY WEBFRONT_REAL_XDG_REGISTERED)
    if(NOT registered)
        cmake_path(SET capture NORMALIZE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../cmake/CaptureRealXdg.cmake")
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${capture}")
        set_property(DIRECTORY PROPERTY WEBFRONT_REAL_XDG_REGISTERED TRUE)
    endif()
endfunction()

# Configure each explicitly registered test and preserve the common helper's environment.
function(webfront_configure_webkitgtk_test name)
    webfront_export_real_xdg()
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
