# WebFront's own test, fuzzing, coverage and sanitizer switches. They carry the project prefix so that an embedding
# parent's ENABLE_TESTING, ENABLE_FUZZING, ENABLE_COVERAGE or ENABLE_SANITIZER_* never build or instrument WebFront's
# tests; a consumer that wants them sets WEBFRONT_ENABLE_<NAME> explicitly.
#
# The former unprefixed names are still read when WebFront is the top-level project, until WebFront 0.7.0, and are
# always removed from the cache. Like the former option() calls, the function runs after project() and reads normal
# variables too (a toolchain file's). A name is "given" by this configure when it is a normal variable or an untyped
# -D entry (an UNINITIALIZED cache entry: no option() declares it yet); a typed BOOL entry was left by an earlier
# configure, or given as -D<NAME>:BOOL, which is read as left. Then:
# - a given prefixed name decides; a former name of another value is ignored with a warning;
# - otherwise a given former name decides and its value is moved to the prefixed option, even over a value an earlier
#   configure left in the cache;
# - otherwise a true former value left by an older build tree is moved, unless an earlier configure already left the
#   prefixed option; a false one is dropped silently: older trees hold the OFF defaults of the former option() calls.
# Moving or repeating a former name prints a deprecation warning.
set(_WEBFRONT_TEST_OPTION_NAMES TESTING FUZZING COVERAGE SANITIZER_ADDRESS SANITIZER_LEAK SANITIZER_UNDEFINED_BEHAVIOR
    SANITIZER_THREAD SANITIZER_MEMORY)
set(_WEBFRONT_FORMER_NAMES_REMOVAL 0.7.0)

# Whether `name` was given by this configure (see above).
function(_webfront_option_given name out)
    set(given OFF)
    if(DEFINED CACHE{${name}})
        set(resolved_value "${${name}}")
        get_property(type CACHE ${name} PROPERTY TYPE)
        if(type STREQUAL "UNINITIALIZED")
            set(given ON)
        else()
            # An equal value does not tell us whether a normal variable shadows the cache. Temporarily change
            # only the cache value: a normal binding keeps its value, while a cache lookup sees the probe.
            # Derive the probe from the resolved value so it cannot equal a normal binding; restore the cache
            # before returning, preserving its type, help text and other properties.
            set(cached_value "$CACHE{${name}}")
            set(probe "${resolved_value};__webfront_option_probe")
            set_property(CACHE ${name} PROPERTY VALUE "${probe}")
            if(NOT "${${name}}" STREQUAL "${probe}")
                set(given ON)
            endif()
            set_property(CACHE ${name} PROPERTY VALUE "${cached_value}")
        endif()
    elseif(DEFINED ${name})
        set(given ON)
    endif()
    set(${out} ${given} PARENT_SCOPE)
endfunction()

function(webfront_declare_test_options is_top_level)
    if(PROJECT_VERSION VERSION_GREATER_EQUAL _WEBFRONT_FORMER_NAMES_REMOVAL)
        message(FATAL_ERROR "WebFront ${PROJECT_VERSION} must no longer read the former ENABLE_* test options: remove "
            "their migration from cmake/WebFrontTestOptions.cmake, as announced in docs/CHANGELOG.md")
    endif()
    foreach(name IN LISTS _WEBFRONT_TEST_OPTION_NAMES)
        _webfront_option_given(WEBFRONT_ENABLE_${name} prefixed_given_${name})
        _webfront_option_given(ENABLE_${name} former_given_${name})
        if(DEFINED CACHE{WEBFRONT_ENABLE_${name}})
            set(prefixed_cached_${name} ON)
        else()
            set(prefixed_cached_${name} OFF)
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
    if(NOT is_top_level)
        return()
    endif()
    foreach(name IN LISTS _WEBFRONT_TEST_OPTION_NAMES)
        if(NOT DEFINED ENABLE_${name})
            continue()
        endif()
        set(former "ENABLE_${name}")
        set(prefixed "WEBFRONT_ENABLE_${name}")
        set(value OFF)
        if(${former})
            set(value ON)
        endif()
        set(current OFF)
        if(${prefixed})
            set(current ON)
        endif()
        unset(${former} CACHE)
        if(NOT former_given_${name} AND NOT value)
            continue()
        endif()
        if(prefixed_given_${name} OR (prefixed_cached_${name} AND NOT former_given_${name}))
            if(value STREQUAL current)
                if(former_given_${name})
                    message(DEPRECATION "${former} is deprecated and will be removed in WebFront "
                        "${_WEBFRONT_FORMER_NAMES_REMOVAL}: use ${prefixed}, which already has this value")
                endif()
            else()
                # Not a deprecation: -Wno-deprecated must not hide that an explicit value has no effect.
                message(WARNING "${former} is ignored: ${prefixed} is set (${current}) and decides. ${former} is "
                    "deprecated and will be removed in WebFront ${_WEBFRONT_FORMER_NAMES_REMOVAL}.")
            endif()
        elseif(prefixed_cached_${name} AND value STREQUAL current)
            message(DEPRECATION "${former} is deprecated and will be removed in WebFront "
                "${_WEBFRONT_FORMER_NAMES_REMOVAL}: use ${prefixed}, which already has this value")
        else()
            message(DEPRECATION "${former} is deprecated and will be removed in WebFront "
                "${_WEBFRONT_FORMER_NAMES_REMOVAL}: its value ${value}, from the command line, a toolchain file or "
                "the cache, now sets ${prefixed}")
            set_property(CACHE ${prefixed} PROPERTY VALUE ${value})
        endif()
    endforeach()
endfunction()
