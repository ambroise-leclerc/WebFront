# Maintenance of the embedded assets of the core: the browser bridge, the fallback and bootstrap pages and the favicon. Normal
# builds consume the checked-in generated headers; the webfront-embedded-assets target regenerates them from their readable
# sources with js/core/EmbedAsset.cmake, in pure CMake, while CTest verifies (per asset, for clear failure attribution)
# that each is current and that the bundles of the environment match the checksums of their inputs.

set(_WEBFRONT_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_webfront_add_embedded_asset source header struct test_name)
    add_custom_command(TARGET webfront-embedded-assets POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DINPUT=${source} -DOUTPUT=${header} -DSTRUCT=${struct}
            -P ${_WEBFRONT_ASSETS_DIR}/EmbedAsset.cmake
        COMMENT "Regenerating ${header}"
        VERBATIM
    )
    if(WEBFRONT_ENABLE_TESTING)
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${source} -DCHECK=${header} -DSTRUCT=${struct}
                -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -P ${_WEBFRONT_ASSETS_DIR}/EmbedAsset.cmake
        )
    endif()
endfunction()

function(webfront_add_embedded_assets)
    find_package(Git QUIET)
    set(root "${_WEBFRONT_ASSETS_DIR}/../..")
    cmake_path(NORMAL_PATH root)
    add_custom_target(webfront-embedded-assets COMMENT "Regenerating the embedded assets of the core")
    _webfront_add_embedded_asset(
        "${root}/js/core/dom-interpreter.mjs" "${root}/include/webfront/system/DomInterpreterData.hpp"
        DomInterpreterData DomInterpreterAssetFresh)
    _webfront_add_embedded_asset(
        "${root}/js/core/WebFront.js" "${root}/include/webfront/system/WebFrontJsData.hpp"
        WebFrontJsData WebFrontJsAssetsFresh)
    _webfront_add_embedded_asset(
        "${root}/js/core/fallback/index.html" "${root}/include/webfront/system/FallbackIndexHtmlData.hpp"
        FallbackIndexHtmlData FallbackIndexHtmlAssetFresh)
    _webfront_add_embedded_asset(
        "${root}/js/core/fallback/webfront-fallback.mjs" "${root}/include/webfront/system/FallbackModuleData.hpp"
        FallbackModuleData FallbackModuleAssetFresh)
    _webfront_add_embedded_asset(
        "${root}/js/core/favicon.ico" "${root}/include/webfront/system/FaviconData.hpp"
        FaviconData FaviconAssetFresh)
    _webfront_add_embedded_asset(
        "${root}/js/core/bootstrap/index.html" "${root}/include/webfront/system/DeveloperIndexHtmlData.hpp"
        DeveloperIndexHtmlData DeveloperIndexHtmlAssetFresh)
    _webfront_add_embedded_asset(
        "${root}/js/core/bootstrap/bootstrap.bundle.mjs" "${root}/include/webfront/system/DeveloperBootstrapData.hpp"
        DeveloperBootstrapData DeveloperBootstrapAssetFresh)

    if(WEBFRONT_ENABLE_TESTING)
        add_test(NAME MinimalCoreAssets
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -P ${root}/test/cmake/CheckMinimalCoreAssets.cmake)
        # The guard must fail on an asset header outside the normative list.
        add_test(NAME MinimalCoreAssetsRejectsUnlisted
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/minimal-core-assets
                -P ${root}/test/cmake/CheckMinimalCoreAssetsNegative.cmake)
        # The renderer: exact format, exit status of a stale or missing header, a checkout reached through a symbolic link.
        add_test(NAME EmbeddedAssetGenerator
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/embedded-asset-generator
                -P ${root}/test/cmake/CheckEmbedAsset.cmake)
        # A source of the environment's bundles changed without rebuilding them is detected: no browser is needed.
        add_test(NAME BundleInputsFresh
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -P ${root}/test/cmake/CheckBundleInputs.cmake)
        add_test(NAME BundleInputsRejectsChange
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/bundle-inputs
                -P ${root}/test/cmake/CheckBundleInputsNegative.cmake)
        # No path of the project depends on a JavaScript runtime or package manager: every file is audited, and the guard is
        # proven by injections.
        add_test(NAME NoNodeDependency
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -P ${root}/test/cmake/CheckNoNode.cmake)
        add_test(NAME NoNodeDependencyRejectsInjections
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/no-node-guard
                -P ${root}/test/cmake/CheckNoNodeNegative.cmake)
        # The terminal and webfront::Commands refuse the same command names.
        add_test(NAME CommandNamesAgree
            COMMAND ${CMAKE_COMMAND} -DROOT=${root} -P ${root}/test/cmake/CheckCommandNames.cmake)
    endif()
endfunction()
