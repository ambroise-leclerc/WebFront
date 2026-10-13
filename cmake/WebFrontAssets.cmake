# Maintenance of the embedded browser bridge and fallback-page assets. Normal builds consume the
# checked-in generated headers; the webfront-embedded-assets target regenerates them from their
# readable sources, while CTest verifies (per asset, for clear failure attribution) that each is current.

set(_WEBFRONT_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_webfront_add_embedded_text_asset source header struct test_name)
    add_custom_command(TARGET webfront-embedded-assets POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${source}
            -DOUTPUT=${header}
            -DSTRUCT=${struct}
            -P ${_WEBFRONT_ASSETS_DIR}/EmbedTextAsset.cmake
        COMMENT "Regenerating ${header}"
        VERBATIM
    )
    if(ENABLE_TESTING)
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND}
                -DINPUT=${source}
                -DCHECK=${header}
                -DSTRUCT=${struct}
                -P ${_WEBFRONT_ASSETS_DIR}/EmbedTextAsset.cmake
        )
    endif()
endfunction()

function(webfront_add_embedded_assets)
    set(root "${_WEBFRONT_ASSETS_DIR}/..")
    cmake_path(NORMAL_PATH root)
    add_custom_target(webfront-embedded-assets COMMENT "Regenerating embedded browser bridge and fallback-page assets")
    _webfront_add_embedded_text_asset(
        "${root}/src/WebFront.js" "${root}/include/system/WebFrontJsData.hpp"
        WebFrontJsData WebFrontJsAssetsFresh)
    _webfront_add_embedded_text_asset(
        "${root}/src/fallback/index.html" "${root}/include/system/FallbackIndexHtmlData.hpp"
        FallbackIndexHtmlData FallbackIndexHtmlAssetFresh)
    _webfront_add_embedded_text_asset(
        "${root}/src/fallback/webfront-fallback.mjs" "${root}/include/system/FallbackModuleData.hpp"
        FallbackModuleData FallbackModuleAssetFresh)
endfunction()
