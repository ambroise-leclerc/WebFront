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

    _webfront_add_embedded_text_asset(
        "${root}/js/core/bootstrap/index.html" "${root}/include/webfront/system/DeveloperIndexHtmlData.hpp"
        DeveloperIndexHtmlData DeveloperIndexHtmlAssetFresh)
    _webfront_add_embedded_text_asset(
        "${root}/js/core/bootstrap/bootstrap.bundle.mjs" "${root}/include/webfront/system/DeveloperBootstrapData.hpp"
        DeveloperBootstrapData DeveloperBootstrapAssetFresh)

    # Terminal component (include/webfront/system/TerminalFS.hpp). xterm.js files are vendored verbatim in
    # third_party/xterm (see its VERSION file); refresh them with cmake/UpdateXterm.cmake.
    _webfront_add_embedded_text_asset(
        "${root}/js/developer/components/terminal/index.html" "${root}/include/webfront/system/TerminalIndexHtmlData.hpp"
        TerminalIndexHtmlData TerminalIndexHtmlAssetFresh)
    _webfront_add_embedded_text_asset(
        "${root}/js/developer/components/terminal/terminal.mjs" "${root}/include/webfront/system/TerminalModuleData.hpp"
        TerminalModuleData TerminalModuleAssetFresh)
    # Browser side of the wf tool manager (include/webfront/system/ToolManagerFS.hpp).
    _webfront_add_embedded_text_asset(
        "${root}/js/developer/components/tools/wf.mjs" "${root}/include/webfront/system/ToolManagerModuleData.hpp"
        ToolManagerModuleData ToolManagerModuleAssetFresh)
    _webfront_add_embedded_text_asset(
        "${root}/third_party/xterm/xterm.mjs" "${root}/include/webfront/system/XtermModuleData.hpp"
        XtermModuleData XtermModuleAssetFresh ON)
    _webfront_add_embedded_text_asset(
        "${root}/third_party/xterm/xterm.css" "${root}/include/webfront/system/XtermCssData.hpp"
        XtermCssData XtermCssAssetFresh ON)
    _webfront_add_embedded_text_asset(
        "${root}/third_party/xterm/addon-fit.mjs" "${root}/include/webfront/system/XtermFitAddonData.hpp"
        XtermFitAddonData XtermFitAddonAssetFresh ON)
    _webfront_add_embedded_text_asset(
        "${root}/third_party/react/runtime.mjs" "${root}/include/webfront/system/ReactRuntimeData.hpp"
        ReactRuntimeData ReactRuntimeAssetFresh ON)
    _webfront_add_embedded_text_asset(
        "${root}/third_party/babel/babel.min.js" "${root}/include/webfront/system/BabelData.hpp"
        BabelData BabelAssetFresh ON)
    foreach(asset IN ITEMS jasmine.js jasmine-html.js jasmine.css boot.js jasmine_favicon.png)
        if(asset STREQUAL "jasmine.js")
            set(struct JasmineJsData)
        elseif(asset STREQUAL "jasmine-html.js")
            set(struct JasmineHtmlData)
        elseif(asset STREQUAL "jasmine.css")
            set(struct JasmineCssData)
        elseif(asset STREQUAL "boot.js")
            set(struct JasmineBootData)
        else()
            set(struct JasmineFaviconData)
        endif()
        if(asset STREQUAL "jasmine_favicon.png")
            set(compress OFF)
        else()
            set(compress ON)
        endif()
        _webfront_add_embedded_text_asset(
            "${root}/third_party/jasmine/${asset}" "${root}/include/webfront/system/${struct}.hpp"
            ${struct} ${struct}AssetFresh ${compress})
    endforeach()
    if(ENABLE_TESTING)
        find_program(WEBFRONT_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
        add_test(NAME DeveloperJavaScript COMMAND "${WEBFRONT_NODE_EXECUTABLE}"
            --test "${root}/js/developer/tools/check.mjs" "${root}/js/developer/tools/distribution-check.mjs")
        set_tests_properties(DeveloperJavaScript PROPERTIES TIMEOUT 30)
        add_test(NAME EmbeddedGzipContents COMMAND "${WEBFRONT_NODE_EXECUTABLE}"
            "${root}/js/developer/vendor/check.mjs")
    endif()
endfunction()
