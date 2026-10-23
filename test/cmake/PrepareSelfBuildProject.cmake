# Gives the self-build tests a fresh copy of the sources that the environment is built from: the wf project of the
# repository (without its lock, which the tests resolve from the local registry), the environment, the tools and scripts,
# the starters, the sources of the core's embedded assets and the pinned header. The build outputs go inside this copy,
# never into the checkout.
# Usage: cmake -DSOURCE=<checkout> -DPROJECT=<directory to create> -P PrepareSelfBuildProject.cmake
file(REMOVE_RECURSE "${PROJECT}")
file(MAKE_DIRECTORY "${PROJECT}" "${PROJECT}/include/webfront/system" "${PROJECT}/js/developer" "${PROJECT}/build-site")
file(COPY "${SOURCE}/webfront.project.json" DESTINATION "${PROJECT}")
file(COPY "${SOURCE}/js/core" DESTINATION "${PROJECT}/js")
foreach(directory environment scripts tools)
    file(COPY "${SOURCE}/js/developer/${directory}" DESTINATION "${PROJECT}/js/developer")
endforeach()
file(COPY "${SOURCE}/examples/life/ui" DESTINATION "${PROJECT}/examples/life")
file(COPY "${SOURCE}/third_party/fflate" DESTINATION "${PROJECT}/third_party")
foreach(header DeveloperReleaseData DeveloperIndexHtmlData DeveloperBootstrapData)
    file(COPY "${SOURCE}/include/webfront/system/${header}.hpp" DESTINATION "${PROJECT}/include/webfront/system")
endforeach()
