# Share the producer/consumer contract with the nested CTest failure regression.
function(webfront_add_developer_session_tests)
    cmake_parse_arguments(ARG "" "PROGRAM;FIXTURES;OUTPUT;REQUIRES;WORKING_DIRECTORY" "PREFIX" ${ARGN})
    add_test(NAME DeveloperBrowserSession COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=${ARG_PROGRAM}" "-DFIXTURES=${ARG_FIXTURES}" "-DOUTPUT=${ARG_OUTPUT}" "-DPREFIX=${ARG_PREFIX}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunDeveloperSession.cmake")
    set_tests_properties(DeveloperBrowserSession PROPERTIES LABELS web-integration TIMEOUT 570 PROCESSORS 4
        FIXTURES_SETUP DeveloperBrowserSession FIXTURES_REQUIRED "${ARG_REQUIRES}")
    foreach(part app scripts typescript jasmine terminal workspace)
        set(name DeveloperBrowserIntegration)
        if(NOT part STREQUAL "app")
            string(APPEND name "-${part}")
        endif()
        add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DCASE_REPORT=${ARG_OUTPUT}/results/${part}.json"
            -DEXPECT_STATUS=0 -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunWfDriver.cmake")
        set_tests_properties(${name} PROPERTIES LABELS web-integration TIMEOUT 180 FIXTURES_REQUIRED DeveloperBrowserSession)
        if(ARG_WORKING_DIRECTORY)
            set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}")
        endif()
    endforeach()
    if(ARG_WORKING_DIRECTORY)
        set_tests_properties(DeveloperBrowserSession PROPERTIES WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}")
    endif()
    set_tests_properties(DeveloperBrowserIntegration PROPERTIES FIXTURES_SETUP DeveloperGenerated)
endfunction()
