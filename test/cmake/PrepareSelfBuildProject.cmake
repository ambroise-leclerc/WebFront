# Gives the self-build tests a fresh copy of the sources that the environment is built from: the wf project of the
# repository, kept in js/developer (without its lock, which the tests resolve from the local registry), the environment, the tools and scripts,
# the starters, the sources of the core's embedded assets, the specs of the developer environment and the pinned header. The build outputs go inside this copy,
# never into the checkout. The site directory is not created: the first staging creates it. The scripts of the test
# registry are copied too, and, with ARCHIVES, the verified archives of its real packages.
# Usage: cmake -DSOURCE=<checkout> -DPROJECT=<directory to create> [-DARCHIVES=<directory>] -P PrepareSelfBuildProject.cmake
file(REMOVE_RECURSE "${PROJECT}")
file(MAKE_DIRECTORY "${PROJECT}" "${PROJECT}/include/webfront/system" "${PROJECT}/js/developer")
file(COPY "${SOURCE}/js/core" DESTINATION "${PROJECT}/js")
file(COPY "${SOURCE}/js/developer/webfront.project.json" DESTINATION "${PROJECT}/js/developer")
foreach(directory environment scripts tools)
    file(COPY "${SOURCE}/js/developer/${directory}" DESTINATION "${PROJECT}/js/developer")
endforeach()
file(COPY "${SOURCE}/examples/life/ui" DESTINATION "${PROJECT}/examples/life")
file(COPY "${SOURCE}/test/web/registry" DESTINATION "${PROJECT}/test/web")
file(COPY "${SOURCE}/test/developer" DESTINATION "${PROJECT}/test")
if(ARCHIVES)
    file(COPY "${ARCHIVES}/" DESTINATION "${PROJECT}/registry-archives")
endif()
file(COPY "${SOURCE}/include/webfront/system/DeveloperReleaseData.hpp" DESTINATION "${PROJECT}/include/webfront/system")
