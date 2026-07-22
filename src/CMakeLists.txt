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

# Browser bridge and fallback-page asset maintenance. Normal builds consume the checked-in
# generated headers; the webfront-embedded-assets target regenerates them from their readable
# sources, while CTest verifies (per asset, for clear failure attribution) that each is current.
function(add_embedded_text_asset source header struct test_name)
    add_custom_command(TARGET webfront-embedded-assets POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${source}
            -DOUTPUT=${header}
            -DSTRUCT=${struct}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedTextAsset.cmake
        COMMENT "Regenerating ${header}"
        VERBATIM
    )
    if(ENABLE_TESTING)
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND}
                -DINPUT=${source}
                -DCHECK=${header}
                -DSTRUCT=${struct}
                -P ${CMAKE_SOURCE_DIR}/cmake/EmbedTextAsset.cmake
        )
    endif()
endfunction()

add_custom_target(webfront-embedded-assets COMMENT "Regenerating embedded browser bridge and fallback-page assets")

add_embedded_text_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/WebFront.js"
    "${CMAKE_SOURCE_DIR}/include/system/WebFrontJsData.hpp"
    WebFrontJsData
    WebFrontJsAssetsFresh
)
add_embedded_text_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/fallback/index.html"
    "${CMAKE_SOURCE_DIR}/include/system/FallbackIndexHtmlData.hpp"
    FallbackIndexHtmlData
    FallbackIndexHtmlAssetFresh
)
add_embedded_text_asset(
    "${CMAKE_CURRENT_SOURCE_DIR}/fallback/webfront-fallback.mjs"
    "${CMAKE_SOURCE_DIR}/include/system/FallbackModuleData.hpp"
    FallbackModuleData
    FallbackModuleAssetFresh
)
