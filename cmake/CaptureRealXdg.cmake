# Keeps the XDG roots a test replaces with its throwaway profile (docs/WEBKITGTK.md, #664). XDG_DATA_HOME and
# XDG_CACHE_HOME are process-wide, so every program the application under test starts would inherit the throwaway
# profile; this records them as the environment that launched the tests has them, so the application can give them back:
# WEBFRONT_TEST_REAL_XDG_DATA_HOME and WEBFRONT_TEST_REAL_XDG_CACHE_HOME, absent when the variable was unset or empty.
#
# Included by ctest when it starts (directory property TEST_INCLUDE_FILES, set by webfront_export_real_xdg in
# test/cmake/WebKitGTKTestEnvironment.cmake) and by every script that replaces the roots itself before doing so:
# cmake/RunWebFrontJasmine.cmake (webfront_add_jasmine_test), test/cmake/RunWfDriver.cmake and
# test/cmake/RunStartupTime.cmake. The first capture wins: it sets WEBFRONT_TEST_XDG_CAPTURED, which the tests and the
# programs they start inherit, so a script or a ctest started inside a test keeps the roots captured at launch instead of
# the throwaway ones. Unset WEBFRONT_TEST_XDG_CAPTURED to capture again.
if(NOT DEFINED ENV{WEBFRONT_TEST_XDG_CAPTURED})
    foreach(webfront_xdg_kind DATA CACHE)
        if(DEFINED ENV{XDG_${webfront_xdg_kind}_HOME})
            # An empty value clears the variable: an empty root is reported as absent.
            set(ENV{WEBFRONT_TEST_REAL_XDG_${webfront_xdg_kind}_HOME} "$ENV{XDG_${webfront_xdg_kind}_HOME}")
        else()
            unset(ENV{WEBFRONT_TEST_REAL_XDG_${webfront_xdg_kind}_HOME})
        endif()
    endforeach()
    unset(webfront_xdg_kind)
    set(ENV{WEBFRONT_TEST_XDG_CAPTURED} 1)
endif()
