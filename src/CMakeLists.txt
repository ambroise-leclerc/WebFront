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

# Browser bridge asset maintenance. Normal builds consume the checked-in header;
# this explicit target regenerates it, while CTest verifies that it is current.
set(WEBFRONT_JS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/WebFront.js")
set(WEBFRONT_JS_HEADER "${CMAKE_SOURCE_DIR}/include/system/WebFrontJsData.hpp")
add_custom_target(webfront-js-assets
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${WEBFRONT_JS_SOURCE}
        -DOUTPUT=${WEBFRONT_JS_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedWebFrontJs.cmake
    COMMENT "Regenerating embedded WebFront.js"
    VERBATIM
)

if(ENABLE_TESTING)
    add_test(NAME WebFrontJsAssetsFresh
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${WEBFRONT_JS_SOURCE}
            -DCHECK=${WEBFRONT_JS_HEADER}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedWebFrontJs.cmake
    )
endif()
