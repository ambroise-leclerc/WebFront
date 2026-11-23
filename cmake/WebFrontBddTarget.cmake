# SPDX-License-Identifier: Apache-2.0 OR CECILL-B
# WebFront::Bdd, the public test framework (include/webfront/testing/Bdd.hpp, docs/BDD.md), and its installation as the
# WebFrontBdd package. The target needs neither WebFront nor stdnet: a project can test with it alone.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Threads: Bdd.hpp runs the waits of requireEventually and uses <thread>.
find_package(Threads REQUIRED)
add_library(WebFront_Bdd INTERFACE)
add_library(WebFront::Bdd ALIAS WebFront_Bdd)
set_target_properties(WebFront_Bdd PROPERTIES EXPORT_NAME Bdd)
target_compile_features(WebFront_Bdd INTERFACE cxx_std_23)
target_link_libraries(WebFront_Bdd INTERFACE Threads::Threads)
target_include_directories(WebFront_Bdd INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# Defines webfront_bdd_discover_tests() for the parent project as well. The function reads no variable of this directory
# (it locates its script with CMAKE_CURRENT_FUNCTION_LIST_FILE): it runs with the variables of its caller.
include("${CMAKE_CURRENT_LIST_DIR}/WebFrontBdd.cmake")

# A project that embeds WebFront owns its own installation: only the top-level project installs the package.
if(WEBFRONT_IS_TOP_LEVEL)
    set(_bdd_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/WebFrontBdd")
    install(TARGETS WebFront_Bdd EXPORT WebFrontBddTargets COMPONENT WebFrontBdd)
    install(FILES "${CMAKE_CURRENT_LIST_DIR}/../include/webfront/testing/Bdd.hpp"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/webfront/testing" COMPONENT WebFrontBdd)
    install(EXPORT WebFrontBddTargets NAMESPACE WebFront:: DESTINATION "${_bdd_cmake_dir}" COMPONENT WebFrontBdd)
    configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/WebFrontBddConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/WebFrontBddConfig.cmake" INSTALL_DESTINATION "${_bdd_cmake_dir}")
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/WebFrontBddConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/WebFrontBddConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/WebFrontBddConfigVersion.cmake"
            "${CMAKE_CURRENT_LIST_DIR}/WebFrontBdd.cmake" DESTINATION "${_bdd_cmake_dir}" COMPONENT WebFrontBdd)
endif()
