# WebFront's own test, fuzzing, coverage and sanitizer switches. They carry the project prefix so that an embedding
# parent's ENABLE_TESTING, ENABLE_FUZZING, ENABLE_COVERAGE or ENABLE_SANITIZER_* never build or instrument WebFront's
# tests; a consumer that wants them sets WEBFRONT_ENABLE_<NAME> explicitly.
#
# The former unprefixed names are still read when WebFront is the top-level project, until WebFront 0.7.0: a value
# given there is moved to the new option (with a deprecation warning) and removed from the cache, so that a later
# -DWEBFRONT_ENABLE_<NAME> is not overridden by a stale entry. A false former value that changes nothing is dropped
# silently.
macro(webfront_declare_test_options is_top_level)
    option(WEBFRONT_ENABLE_TESTING "Build WebFront's tests" OFF)
    option(WEBFRONT_ENABLE_FUZZING "Build WebFront's fuzzers" OFF)
    option(WEBFRONT_ENABLE_COVERAGE "Instrument WebFront's tests for GCC/Clang coverage" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_ADDRESS "Build WebFront's tests with the address sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_LEAK "Build WebFront's tests with the leak sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_UNDEFINED_BEHAVIOR "Build WebFront's tests with the undefined behavior sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_THREAD "Build WebFront's tests with the thread sanitizer" OFF)
    option(WEBFRONT_ENABLE_SANITIZER_MEMORY "Build WebFront's tests with the memory sanitizer" OFF)
    foreach(_webfront_name TESTING FUZZING COVERAGE SANITIZER_ADDRESS SANITIZER_LEAK SANITIZER_UNDEFINED_BEHAVIOR
            SANITIZER_THREAD SANITIZER_MEMORY)
        if(${is_top_level} AND DEFINED CACHE{ENABLE_${_webfront_name}})
            set(_webfront_value "$CACHE{ENABLE_${_webfront_name}}")
            # Build trees configured before the rename hold the former OFF defaults: drop those without noise.
            if(NOT _webfront_value AND NOT WEBFRONT_ENABLE_${_webfront_name})
                unset(ENABLE_${_webfront_name} CACHE)
                continue()
            endif()
            message(DEPRECATION "ENABLE_${_webfront_name} is deprecated and will be removed in WebFront 0.7.0: "
                "use WEBFRONT_ENABLE_${_webfront_name} (set to ${_webfront_value} for this build)")
            unset(ENABLE_${_webfront_name} CACHE)
            set_property(CACHE WEBFRONT_ENABLE_${_webfront_name} PROPERTY VALUE "${_webfront_value}")
        endif()
    endforeach()
    unset(_webfront_name)
    unset(_webfront_value)
endmacro()
