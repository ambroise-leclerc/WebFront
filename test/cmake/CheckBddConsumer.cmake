# Installs the WebFrontBdd package of BUILD_DIR into WORK, then configures, builds and runs a project that finds it and
# registers its own scenarios with webfront_bdd_discover_tests (#647).
#   cmake -DROOT=<checkout> -DBUILD_DIR=<configured tree> -DWORK=<scratch> -DGENERATOR=<g> -DCXX_COMPILER=<c>
#         [-DCONFIG=<config>] -P test/cmake/CheckBddConsumer.cmake
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_step description)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed (${result}):\n${output}${errors}")
    endif()
    set(step_output "${output}" PARENT_SCOPE)
endfunction()

if(NOT CONFIG)
    set(CONFIG Release)
endif()
run_step("Installing the package" "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config ${CONFIG} --component WebFrontBdd --prefix "${WORK}/prefix")
foreach(installed IN ITEMS include/webfront/testing/Bdd.hpp lib/cmake/WebFrontBdd/WebFrontBddConfig.cmake
                           lib/cmake/WebFrontBdd/WebFrontBddTargets.cmake lib/cmake/WebFrontBdd/WebFrontBdd.cmake)
    if(NOT EXISTS "${WORK}/prefix/${installed}")
        message(FATAL_ERROR "The installation lacks ${installed}")
    endif()
endforeach()

run_step("Configuring the consumer" "${CMAKE_COMMAND}" -S "${ROOT}/test/cmake/bdd-consumer" -B "${WORK}/build" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_PREFIX_PATH=${WORK}/prefix" -DCMAKE_BUILD_TYPE=${CONFIG})
run_step("Building the consumer" "${CMAKE_COMMAND}" --build "${WORK}/build" --config ${CONFIG})

run_step("Listing the cases" "${CMAKE_CTEST_COMMAND}" -N -C ${CONFIG} --test-dir "${WORK}/build")
foreach(name IN ITEMS "Consumer.Scenario: Addition" "Consumer.Scenario: A [bracketed; title] stays whole"
                      "Consumer.Scenario: Third" "ConsumerShards-0" "ConsumerShards-1")
    string(FIND "${step_output}" ": ${name}\n" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "CTest does not list \"${name}\":\n${step_output}")
    endif()
endforeach()
if(NOT step_output MATCHES "Total Tests: 5\n")
    message(FATAL_ERROR "CTest should list 5 cases:\n${step_output}")
endif()
run_step("Running the cases" "${CMAKE_CTEST_COMMAND}" --test-dir "${WORK}/build" -C ${CONFIG} --output-on-failure --no-tests=error)
