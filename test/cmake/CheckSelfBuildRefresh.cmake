# The bootstrap bundles and their checksums that wf regenerates from the sources must be the checked-in ones, and the core
# headers that embed the bundles, rendered from them by js/core/EmbedAsset.cmake, too: the proof that the checked-in bundles
# and headers are rebuilt from the sources by wf and CMake alone. The pinned header and its checksum are the exceptions:
# the sources build a version that is not published yet, so the refresh writes the header of that version.
# Usage: cmake -DSOURCE=<checkout> -DPROJECT=<self-build project> -DOUTPUT=<directory of the build> -P CheckSelfBuildRefresh.cmake
set(pinned "include/webfront/system/DeveloperReleaseData.hpp")
foreach(name js/core/bootstrap/bootstrap.bundle.mjs js/developer/environment/packages.bundle.mjs
        js/developer/environment/PACKAGES-LICENSE.txt)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SOURCE}/${name}" "${PROJECT}/${name}" RESULT_VARIABLE different)
    if(different)
        message(FATAL_ERROR "${name} rebuilt by wf differs from the checked-in file")
    endif()
endforeach()
foreach(asset "js/core/bootstrap/index.html;DeveloperIndexHtmlData" "js/core/bootstrap/bootstrap.bundle.mjs;DeveloperBootstrapData")
    list(GET asset 0 input)
    list(GET asset 1 struct)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DINPUT=${PROJECT}/${input}" -DSTRUCT=${struct}
        "-DOUTPUT=${PROJECT}/rendered/${struct}.hpp" -P "${SOURCE}/js/core/EmbedAsset.cmake" RESULT_VARIABLE rendered)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SOURCE}/include/webfront/system/${struct}.hpp"
        "${PROJECT}/rendered/${struct}.hpp" RESULT_VARIABLE different)
    if(NOT rendered EQUAL 0 OR different)
        message(FATAL_ERROR "${struct}.hpp rendered from the bundle rebuilt by wf differs from the checked-in header")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${PROJECT}/${pinned}" "${PROJECT}/${OUTPUT}/DeveloperReleaseData.hpp" RESULT_VARIABLE different)
if(different)
    message(FATAL_ERROR "The refresh and the build wrote different pinned headers")
endif()

file(READ "${SOURCE}/js/developer/environment/bundles.json" expected)
file(READ "${PROJECT}/js/developer/environment/bundles.json" actual)
string(JSON expected REMOVE "${expected}" "${pinned}")
string(JSON actual REMOVE "${actual}" "${pinned}")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "bundles.json rebuilt by wf differs from the checked-in checksums")
endif()
