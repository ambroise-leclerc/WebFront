# SPDX-License-Identifier: Apache-2.0 OR CECILL-B
# The WEBFRONT_TEST_TIMEOUT_FACTOR of Bdd.hpp, as CMake reads it to size the CTest limits of the unit tests.
#
# webfront_unit_timeouts(<text> <unit-timeout> <shard-timeout> <environment>) sets the three variables in the caller.
# The grammar is that of parseFactor(): up to six digits with an optional fraction, such as 2 or 2.5, never zero; anything
# else leaves the defaults. A scenario may wait several times for a condition that never holds, so the limit leaves room for
# two of the 30 s waits, times the factor rounded up; a factor of 1 or less leaves it at 60 s (600 s for a shard). The
# environment is the property that gives the factor to the tests, empty when it is not set.
function(webfront_unit_timeouts text unit_timeout shard_timeout environment)
  set(factor 1)
  set(test_environment "")
  # (CMake regular expressions have no {1,6}.)
  if(text MATCHES "^([0-9][0-9]?[0-9]?[0-9]?[0-9]?[0-9]?)(\\.[0-9]+)?$")
    set(whole "${CMAKE_MATCH_1}")
    set(fraction "${CMAKE_MATCH_2}")
    if(NOT text MATCHES "^0+(\\.0+)?$")
      set(factor ${whole})
      if(NOT fraction MATCHES "^(\\.0+)?$")
        math(EXPR factor "${factor} + 1")
      endif()
      set(test_environment ENVIRONMENT "WEBFRONT_TEST_TIMEOUT_FACTOR=${text}")
    endif()
  endif()
  if(factor GREATER 1)
    math(EXPR unit "60 + 60 * ${factor}")
    math(EXPR shard "600 + 600 * ${factor}")
  else()
    set(unit 60)
    set(shard 600)
  endif()
  set(${unit_timeout} ${unit} PARENT_SCOPE)
  set(${shard_timeout} ${shard} PARENT_SCOPE)
  set(${environment} ${test_environment} PARENT_SCOPE)
endfunction()
