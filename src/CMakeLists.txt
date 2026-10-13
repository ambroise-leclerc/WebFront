# ==============================================================================
# WebFrontApp Target Configuration
# ==============================================================================

# Add CMake modules path
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Include CEF support module
include(CEFSupport)

# Newcomer examples. Executables land in <build>/src; run them from the build directory.
#   ./src/HelloWorld       embedded starter page, no assets
#   ./src/WebFrontGreeter  editable UI (src/greeter) calling a deduced C++ callback
add_executable(HelloWorld HelloWorld.cpp)
target_link_libraries(HelloWorld PRIVATE WebFront WebFront_warnings WebFront_options)

add_executable(WebFrontGreeter Greeter.cpp)
target_link_libraries(WebFrontGreeter PRIVATE WebFront WebFront_warnings WebFront_options)
# Publish the greeter assets to <build>/src/greeter, whatever the build configuration, so the
# documented relative document root works. The target runs on every build and only touches files
# that changed, so edits to index.html or greeter.mjs are picked up without relinking the executable.
add_custom_target(WebFrontGreeterAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/greeter" "${CMAKE_CURRENT_BINARY_DIR}/greeter"
    COMMENT "Copying greeter assets"
    VERBATIM
)
add_dependencies(WebFrontGreeter WebFrontGreeterAssets)

# Native-module and React demo (src/module-demo.html, src/react.html)
add_executable(WebFrontApp ModuleDemo.cpp)
target_link_libraries(WebFrontApp PRIVATE WebFront WebFront_warnings WebFront_options)

# ==============================================================================
# CEF Integration
# ==============================================================================

# Setup complete CEF integration for every example, so switching an example's alias to
# webfront::frontend::CEFFrontend works in a -DWEBFRONT_EMBED_CEF=ON build. It is a no-op otherwise.
# This includes:
# - CEF compilation configuration and linking
# - Platform-specific runtime file deployment  
# - Resource file copying (with Linux ICU fix)
# - Locale directory setup
setup_cef_for_target(HelloWorld)
setup_cef_for_target(WebFrontGreeter)
setup_cef_for_target(WebFrontApp)
