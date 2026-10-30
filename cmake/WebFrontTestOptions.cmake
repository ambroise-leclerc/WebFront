# WebFront's own test, fuzzing, coverage and sanitizer switches. They carry the project prefix so that an embedding
# parent's ENABLE_TESTING, ENABLE_FUZZING, ENABLE_COVERAGE or ENABLE_SANITIZER_* never build or instrument WebFront's
# tests; a consumer that wants them sets WEBFRONT_ENABLE_<NAME> explicitly.
#
# The former unprefixed names are still read when WebFront is the top-level project, until WebFront 0.7.0, and are
# always removed from the cache:
# - a prefixed option already in the cache (given with -D, or created by an earlier configure) decides; a true former
#   value is then ignored with a warning;
# - otherwise, a true former value (from the command line or left in the cache of an older build tree) is moved to the
#   prefixed option with a deprecation warning.
# A false former value is dropped silently: older build trees hold the OFF defaults of the former option() calls,
# which the user never chose.
macro(webfront_declare_test_options is_top_level)
    set(_webfront_names TESTING FUZZING COVERAGE SANITIZER_ADDRESS SANITIZER_LEAK SANITIZER_UNDEFINED_BEHAVIOR
        SANITIZER_THREAD SANITIZER_MEMORY)
    set(_webfront_chosen "")
    foreach(_webfront_name IN LISTS _webfront_names)
        if(DEFINED CACHE{WEBFRONT_ENABLE_${_webfront_name}})
            list(APPEND _webfront_chosen ${_webfront_name})
        endif()
    endforeach()
    option(WEBFRONT_ENABLE_TESTING "Build WebFront's tests" OFF)
    option(WEBFRONT_ENABLE_FUZZING "Build WebFront's fuzzers" OFF)
    option(WEBFRONT_ENABLE_COVERAGE "Instrument WebFront's tests for GCC/Clang coverage" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_ADDRESS "Build WebFront's tests with the address sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_LEAK "Build WebFront's tests with the leak sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_UNDEFINED_BEHAVIOR "Build WebFront's tests with the undefined behavior sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_THREAD "Build WebFront's tests with the thread sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_MEMORY "Build WebFront's tests with the memory sanitizer" OFF)
    foreach(_webfront_name IN LISTS _webfront_names)
        if(${is_top_level} AND DEFINED CACHE{ENABLE_${_webfront_name}})
            set(_webfront_value "$CACHE{ENABLE_${_webfront_name}}")
            unset(ENABLE_${_webfront_name} CACHE)
            if(NOT _webfront_value)
                continue()
            endif()
            if(_webfront_name IN_LIST _webfront_chosen)
                message(DEPRECATION "ENABLE_${_webfront_name} is deprecated and ignored: "
                    "WEBFRONT_ENABLE_${_webfront_name} is set (${WEBFRONT_ENABLE_${_webfront_name}}) and decides. "
                    "ENABLE_${_webfront_name} will be removed in WebFront 0.7.0.")
            else()
                message(DEPRECATION "ENABLE_${_webfront_name} is deprecated and will be removed in WebFront 0.7.0: "
                    "its value ${_webfront_value}, from the command line or the cache, now sets "
                    "WEBFRONT_ENABLE_${_webfront_name}")
                set_property(CACHE WEBFRONT_ENABLE_${_webfront_name} PROPERTY VALUE "${_webfront_value}")
            endif()
        endif()
    endforeach()
    unset(_webfront_names)
    unset(_webfront_chosen)
    unset(_webfront_name)
    unset(_webfront_value)
endmacro()
