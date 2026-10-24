# The terminal (js/developer/environment/commands.mjs) and webfront::Commands (include/webfront/Commands.hpp) must refuse the
# same command names to an application: a name one accepts and the other handles would never reach the application.
#   cmake -DROOT=<checkout> -P test/cmake/CheckCommandNames.cmake
cmake_minimum_required(VERSION 3.31)

if(NOT ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

function(reserved_names file pattern output)
    file(READ "${ROOT}/${file}" text)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${file} must declare reservedNames")
    endif()
    string(REGEX MATCHALL "[\"']([^\"']*)[\"']" quoted "${CMAKE_MATCH_1}")
    set(names "")
    foreach(item IN LISTS quoted)
        string(REGEX REPLACE "^[\"']|[\"']$" "" name "${item}")
        list(APPEND names "${name}")
    endforeach()
    list(SORT names)
    set(${output} "${names}" PARENT_SCOPE)
endfunction()

reserved_names(js/developer/environment/commands.mjs "reservedNames = \\[([^]]*)\\]" terminal)
reserved_names(include/webfront/Commands.hpp "reservedNames\\{([^}]*)\\}" cpp)
if(NOT terminal)
    message(FATAL_ERROR "commands.mjs declares no reserved name")
endif()
if(NOT terminal STREQUAL cpp)
    message(FATAL_ERROR "The terminal reserves '${terminal}' but webfront::Commands reserves '${cpp}'")
endif()
message(STATUS "Both reserve: ${terminal}")
