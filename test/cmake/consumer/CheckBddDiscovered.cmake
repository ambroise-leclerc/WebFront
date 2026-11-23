# The post-build step of webfront_bdd_discover_tests wrote the case of the scenario into DIRECTORY.
file(GLOB generated "${DIRECTORY}/bdd_consumer_tests*.cmake")
if(NOT generated)
    message(FATAL_ERROR "The discovery of bdd_consumer wrote no test file in ${DIRECTORY}")
endif()
file(READ "${generated}" content)
if(NOT content MATCHES "Embedded\.Scenario: Embedded WebFront")
    message(FATAL_ERROR "The discovery did not register the scenario:\n${content}")
endif()
