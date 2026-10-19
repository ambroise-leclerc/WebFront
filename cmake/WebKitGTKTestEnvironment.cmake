include_guard(GLOBAL)

# Configure each explicitly registered test and preserve the common helper's environment.
function(webfront_configure_webkitgtk_test name)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-data"
        "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/webkitgtk-cache"
        "GDK_BACKEND=x11" "GSK_RENDERER=cairo" "LIBGL_ALWAYS_SOFTWARE=1")
endfunction()
