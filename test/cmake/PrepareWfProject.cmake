# Gives the driver tests a fresh copy of the fixture project.
# Usage: cmake -DSOURCE=<fixture project> -DPROJECT=<directory to create> -P PrepareWfProject.cmake
file(REMOVE_RECURSE "${PROJECT}")
file(COPY "${SOURCE}/" DESTINATION "${PROJECT}")
