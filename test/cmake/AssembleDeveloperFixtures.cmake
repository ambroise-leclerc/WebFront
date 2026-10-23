# Assembles the directory of the developer browser tests from what wf built: the local npm registry, the environment
# built from the current sources (archive, starters, provision.json) and the pages and specs of the tests.
# Usage: cmake -DSOURCE=<checkout> -DPROJECT=<copy of the sources built by wf> -DOUTPUT=<directory to create> -P AssembleDeveloperFixtures.cmake
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(directory build-registry build-distribution)
    if(NOT EXISTS "${PROJECT}/${directory}")
        message(FATAL_ERROR "${PROJECT}/${directory} is missing: wf must build the registry and the environment first")
    endif()
endforeach()
file(COPY "${PROJECT}/build-distribution/" DESTINATION "${OUTPUT}")
file(COPY "${PROJECT}/build-registry/" DESTINATION "${OUTPUT}/registry")
file(COPY_FILE "${SOURCE}/test/web/developer-specs.mjs" "${OUTPUT}/run.mjs")
file(COPY_FILE "${SOURCE}/test/web/developer-session.html" "${OUTPUT}/session.html")
file(COPY_FILE "${SOURCE}/test/web/developer-session.mjs" "${OUTPUT}/session.mjs")
file(COPY_FILE "${SOURCE}/js/developer/tools/starters/pi/main.tsx" "${OUTPUT}/main.tsx")
file(COPY_FILE "${SOURCE}/js/developer/tools/starters/pi/style.css" "${OUTPUT}/style.css")
