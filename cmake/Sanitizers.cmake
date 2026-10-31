function(enable_sanitizers target)
  cmake_parse_arguments(SANITIZER_OPTIONS "NO_COVERAGE" "" "" ${ARGN})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
    if(WEBFRONT_ENABLE_COVERAGE AND NOT SANITIZER_OPTIONS_NO_COVERAGE)
      # The server tests run threads: non-atomic counters can race and give lcov negative counts.
      target_compile_options(${target} INTERFACE --coverage -fprofile-update=atomic -O0 -g)
      target_link_libraries(${target} INTERFACE --coverage)
    endif()

    set(SANITIZERS "")

    if(WEBFRONT_ENABLE_SANITIZER_ADDRESS)
      list(APPEND SANITIZERS "address")
    endif()

    if(WEBFRONT_ENABLE_SANITIZER_LEAK)
      list(APPEND SANITIZERS "leak")
    endif()

    if(WEBFRONT_ENABLE_SANITIZER_UNDEFINED_BEHAVIOR)
      list(APPEND SANITIZERS "undefined")
    endif()

    if(WEBFRONT_ENABLE_SANITIZER_THREAD)
      if("address" IN_LIST SANITIZERS OR "leak" IN_LIST SANITIZERS)
        message(WARNING "Thread sanitizer does not work with Address and Leak sanitizer enabled")
      else()
        list(APPEND SANITIZERS "thread")
      endif()
    endif()

    if(WEBFRONT_ENABLE_SANITIZER_MEMORY AND CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
      message(WARNING "Memory sanitizer requires all the code (including libc++) to be MSan-instrumented otherwise it reports false positives")
      if("address" IN_LIST SANITIZERS
         OR "thread" IN_LIST SANITIZERS
         OR "leak" IN_LIST SANITIZERS)
        message(WARNING "Memory sanitizer does not work with Address, Thread and Leak sanitizer enabled")
      else()
        list(APPEND SANITIZERS "memory")
      endif()
    endif()

    list(
      JOIN
      SANITIZERS
      ","
      LIST_OF_SANITIZERS)

  endif()

  if(LIST_OF_SANITIZERS)
    if(NOT
       "${LIST_OF_SANITIZERS}"
       STREQUAL
       "")
      target_compile_options(${target} INTERFACE -fsanitize=${LIST_OF_SANITIZERS} -fno-omit-frame-pointer)
      if("undefined" IN_LIST SANITIZERS)
        target_compile_options(${target} INTERFACE -fno-sanitize-recover=undefined)
      endif()
      target_link_options(${target} INTERFACE -fsanitize=${LIST_OF_SANITIZERS})
    endif()
  endif()

endfunction()
