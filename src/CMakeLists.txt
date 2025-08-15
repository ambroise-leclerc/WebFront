# ==============================================================================
# WebFrontApp Target Configuration
# ==============================================================================

find_package(Threads)

# Add CMake modules path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Include CEF support module
include(CEFSupport)

# Create main executable
add_executable(WebFrontApp HelloWorld.cpp)

# Link core libraries
target_link_libraries(WebFrontApp PRIVATE 
    WebFront 
    Threads::Threads
)

# ==============================================================================
# CEF Integration
# ==============================================================================

# Setup complete CEF integration for WebFrontApp
# This includes:
# - CEF compilation configuration and linking
# - Platform-specific runtime file deployment  
# - Resource file copying (with Linux ICU fix)
# - Locale directory setup
setup_cef_for_target(WebFrontApp)

# ==============================================================================
# Additional Build Rules
# ==============================================================================

# TypeScript compilation
add_custom_command(
    OUTPUT WebFront.js
    COMMAND tsc
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})