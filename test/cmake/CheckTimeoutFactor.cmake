# SPDX-License-Identifier: Apache-2.0 OR CECILL-B
# Checks how the unit tests' CTest limits follow WEBFRONT_TEST_TIMEOUT_FACTOR. Run with -DROOT=<source directory> -P.
include(${ROOT}/test/unit/TimeoutFactor.cmake)

set(failures "")
function(expect text unit shard environment)
  webfront_unit_timeouts("${text}" got_unit got_shard got_environment)
  if(NOT "${got_unit}" STREQUAL "${unit}" OR NOT "${got_shard}" STREQUAL "${shard}" OR NOT "${got_environment}" STREQUAL "${environment}")
    set(failures "${failures}\n  '${text}': ${got_unit} ${got_shard} '${got_environment}', expected ${unit} ${shard} '${environment}'" PARENT_SCOPE)
  endif()
endfunction()

expect("" 60 600 "")
expect("1" 60 600 "ENVIRONMENT;WEBFRONT_TEST_TIMEOUT_FACTOR=1")
expect("0.5" 60 600 "ENVIRONMENT;WEBFRONT_TEST_TIMEOUT_FACTOR=0.5")
expect("2" 180 1800 "ENVIRONMENT;WEBFRONT_TEST_TIMEOUT_FACTOR=2")
expect("2.5" 240 2400 "ENVIRONMENT;WEBFRONT_TEST_TIMEOUT_FACTOR=2.5")
expect("3.0" 240 2400 "ENVIRONMENT;WEBFRONT_TEST_TIMEOUT_FACTOR=3.0")
expect("10" 660 6600 "ENVIRONMENT;WEBFRONT_TEST_TIMEOUT_FACTOR=10")
foreach(ignored 1e2 .5 2. 0 0.0 -2 x 3x 1234567)
  expect("${ignored}" 60 600 "")
endforeach()

if(failures)
  message(FATAL_ERROR "The timeout factor is not read as expected:${failures}")
endif()
