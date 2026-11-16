cmake_minimum_required(VERSION 3.31)
project(WebFrontDistributionConsumer LANGUAGES NONE)
include("${WEBFRONT_ROOT}/cmake/WebFrontDistribution.cmake")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/main.cpp" "
#include <webfront/frontend/WebKitGTKPolicy.hpp>
int main() {
#if defined(__linux__)
    using namespace webfront::frontend::detail::webkitgtk;
    unsetenv(\"WEBFRONT_REQUIRE_WEBKIT_SANDBOX\");
    setenv(\"WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS\", \"1\", 1);
    try { prepareSandboxLaunch(); }
    catch (const std::runtime_error&) {
#ifdef WEBFRONT_DISTRIBUTION
        return 0;
#else
        return 1;
#endif
    }
#ifdef WEBFRONT_DISTRIBUTION
    return 2;
#endif
#endif
    return 0;
}
")
foreach(name app development)
    add_executable(${name} "${CMAKE_CURRENT_BINARY_DIR}/main.cpp")
    target_include_directories(${name} PRIVATE "${WEBFRONT_ROOT}/include")
endforeach()
set(installed "/opt/WebFront Example/bin/app")
if(CASE STREQUAL "relative")
    set(installed "bin/app")
elseif(CASE STREQUAL "pattern")
    set(installed "/usr/bin/*")
elseif(CASE STREQUAL "traversal")
    set(installed "/usr/bin/../app")
elseif(CASE STREQUAL "missing-target")
    webfront_use_distribution(absent)
elseif(CASE STREQUAL "library")
    add_library(not_an_application INTERFACE)
    webfront_use_distribution(not_an_application)
endif()
webfront_use_distribution(app)
webfront_generate_webkitgtk_profile(TARGET development EXECUTABLE /usr/bin/development OUTPUT profiles/development)
webfront_generate_webkitgtk_profile(TARGET app EXECUTABLE "${installed}" OUTPUT profiles/webfront.app)
get_target_property(definitions app COMPILE_DEFINITIONS)
get_target_property(development_definitions development COMPILE_DEFINITIONS)
if(NOT "WEBFRONT_DISTRIBUTION=1" IN_LIST definitions OR development_definitions)
    message(FATAL_ERROR "The distribution policy must affect only the selected executable")
endif()
# Invalid requests fail before compiler discovery; only the positive consumer needs a toolchain.
enable_language(CXX)
foreach(name app development)
    target_compile_features(${name} PRIVATE cxx_std_23)
endforeach()
# Stage the package's profile without installing or loading anything on the host.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/profiles/webfront.app" DESTINATION etc/apparmor.d)
enable_testing()
add_test(NAME distribution-policy COMMAND app)
add_test(NAME development-policy COMMAND development)
