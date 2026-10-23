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
            -DGZIP=${ARGN}
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
                -DGZIP=${ARGN}
                -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
                -P ${_WEBFRONT_ASSETS_DIR}/EmbedTextAsset.cmake
        )
    endif()
endfunction()

# The favicon is binary and small: a pure CMake renderer, with no Node.js, produces its header.
function(_webfront_add_embedded_binary_asset source header struct test_name)
    add_custom_command(TARGET webfront-embedded-assets POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DINPUT=${source} -DOUTPUT=${header} -DSTRUCT=${struct}
            -P ${_WEBFRONT_ASSETS_DIR}/EmbedBinaryAsset.cmake
        COMMENT "Regenerating ${header}"
        VERBATIM
    )
    if(ENABLE_TESTING)
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${source} -DCHECK=${header} -DSTRUCT=${struct}
                -P ${_WEBFRONT_ASSETS_DIR}/EmbedBinaryAsset.cmake)
    endif()
endfunction()

function(webfront_add_embedded_assets)
    find_package(Git QUIET)
    set(root "${_WEBFRONT_ASSETS_DIR}/../..")
    cmake_path(NORMAL_PATH root)
    add_custom_target(webfront-embedded-assets COMMENT "Regenerating embedded browser bridge and fallback-page assets")
    _webfront_add_embedded_text_asset(
        "${root}/js/core/WebFront.js" "${root}/include/webfront/system/WebFrontJsData.hpp"
        WebFrontJsData WebFrontJsAssetsFresh)
    _webfront_add_embedded_text_asset(
        "${root}/js/core/fallback/index.html" "${root}/include/webfront/system/FallbackIndexHtmlData.hpp"
        FallbackIndexHtmlData FallbackIndexHtmlAssetFresh)
    _webfront_add_embedded_text_asset(
        "${root}/js/core/fallback/webfront-fallback.mjs" "${root}/include/webfront/system/FallbackModuleData.hpp"
        FallbackModuleData FallbackModuleAssetFresh)
    _webfront_add_embedded_binary_asset(
        "${root}/js/core/favicon.ico" "${root}/include/webfront/system/FaviconData.hpp"
        FaviconData FaviconAssetFresh)

    _webfront_add_embedded_text_asset(
        "${root}/js/core/bootstrap/index.html" "${root}/include/webfront/system/DeveloperIndexHtmlData.hpp"
        DeveloperIndexHtmlData DeveloperIndexHtmlAssetFresh)
    _webfront_add_embedded_text_asset(
        "${root}/js/core/bootstrap/bootstrap.bundle.mjs" "${root}/include/webfront/system/DeveloperBootstrapData.hpp"
        DeveloperBootstrapData DeveloperBootstrapAssetFresh)

    if(ENABLE_TESTING)
        add_test(NAME MinimalCoreAssets
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -P ${root}/test/cmake/CheckMinimalCoreAssets.cmake)
        # The guard must fail on an asset header outside the normative list.
        add_test(NAME MinimalCoreAssetsRejectsUnlisted
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/minimal-core-assets
                -P ${root}/test/cmake/CheckMinimalCoreAssetsNegative.cmake)
        find_program(WEBFRONT_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
        add_test(NAME DeveloperJavaScript COMMAND "${WEBFRONT_NODE_EXECUTABLE}"
            --test "${root}/js/developer/tools/check.mjs" "${root}/js/developer/tools/distribution-check.mjs"
            "${root}/js/developer/tools/manifest-check.mjs" "${root}/js/developer/tools/restore-check.mjs" "${root}/js/developer/tools/scripts-check.mjs")
        set_tests_properties(DeveloperJavaScript PROPERTIES TIMEOUT 30)
        add_test(NAME EmbeddedAssetGenerator COMMAND ${CMAKE_COMMAND} -E env
            "WEBFRONT_TEST_CMAKE=${CMAKE_COMMAND}" "${WEBFRONT_NODE_EXECUTABLE}"
            --test "${root}/js/developer/vendor/embed.test.mjs")
        set_tests_properties(EmbeddedAssetGenerator PROPERTIES TIMEOUT 30)
    endif()
endfunction()
