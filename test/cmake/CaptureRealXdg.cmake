# Keeps the XDG roots a test replaces with its throwaway WebKitGTK profile (docs/WEBKITGTK.md). Included when it starts
# running, by ctest (directory property TEST_INCLUDE_FILES, set by webfront_export_real_xdg) and by the scripts that
# replace the variables themselves, it records the values of XDG_DATA_HOME and XDG_CACHE_HOME as the environment that
# launched the tests has them, so the programs the application starts can be given them back:
# WEBFRONT_TEST_REAL_XDG_DATA_HOME and WEBFRONT_TEST_REAL_XDG_CACHE_HOME are absent when the variable was unset or empty.
# The first capture wins (WEBFRONT_TEST_XDG_CAPTURED), so a script run by a test, whose XDG roots ctest already
# replaced, keeps the values captured at launch.
if(NOT DEFINED ENV{WEBFRONT_TEST_XDG_CAPTURED})
    foreach(webfront_xdg_kind DATA CACHE)
        if(DEFINED ENV{XDG_${webfront_xdg_kind}_HOME})
            set(ENV{WEBFRONT_TEST_REAL_XDG_${webfront_xdg_kind}_HOME} "$ENV{XDG_${webfront_xdg_kind}_HOME}")
        else()
            unset(ENV{WEBFRONT_TEST_REAL_XDG_${webfront_xdg_kind}_HOME})
        endif()
    endforeach()
    set(ENV{WEBFRONT_TEST_XDG_CAPTURED} 1)
endif()
