if(NOT APPLE)
    message(FATAL_ERROR "WEBFRONT_EMBED_WKWEBVIEW is supported only on macOS")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "WEBFRONT_EMBED_WKWEBVIEW requires Clang with Apple Blocks support")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.3")
    message(FATAL_ERROR "WEBFRONT_EMBED_WKWEBVIEW requires CMAKE_OSX_DEPLOYMENT_TARGET >= 13.3")
endif()
find_library(WEBFRONT_COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(WEBFRONT_WEBKIT_FRAMEWORK WebKit REQUIRED)
target_compile_definitions(WebFront INTERFACE WEBFRONT_EMBED_WKWEBVIEW)
target_compile_options(WebFront INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:-fblocks>")
target_link_libraries(WebFront INTERFACE "${WEBFRONT_COCOA_FRAMEWORK}" "${WEBFRONT_WEBKIT_FRAMEWORK}")

function(webfront_add_wkwebview_tests)
    get_filename_component(webtest_source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../test/web" ABSOLUTE)
    add_executable(wkwebview_lifecycle "${webtest_source_dir}/WKWebViewLifecycle.cpp"
        "${webtest_source_dir}/WKWebViewHeaderConsumer.cpp")
    target_link_libraries(wkwebview_lifecycle PRIVATE WebFront WebFront_warnings WebFront_options)
    foreach(mode repeated-show main-thread native-close quit navigation-diagnostic startup-timeout settings redirect-policy missing-response committed-navigation agent-application)
        add_test(NAME WKWebViewLifecycle-${mode} COMMAND wkwebview_lifecycle ${mode})
        webfront_set_browser_test_properties("WKWebViewLifecycle-${mode}" "${webtest_source_dir}" 90)
    endforeach()
    add_executable(wkwebview_transport_benchmark "${webtest_source_dir}/WKWebViewTransportBenchmark.cpp")
    target_link_libraries(wkwebview_transport_benchmark PRIVATE WebFront_test_hardening WebFront WebFront_warnings WebFront_options)
    add_dependencies(webtest wkwebview_transport_benchmark)
    add_test(NAME WKWebViewTransportBenchmark COMMAND wkwebview_transport_benchmark
        "${CMAKE_CURRENT_BINARY_DIR}/wkwebview-transport-benchmark.json")
    webfront_set_browser_test_properties(WKWebViewTransportBenchmark "${webtest_source_dir}" 300)
    set_property(TEST WKWebViewTransportBenchmark APPEND PROPERTY LABELS performance)
    add_test(NAME WKWebViewTransportContracts COMMAND wkwebview_transport_benchmark --contracts)
    webfront_set_browser_test_properties(WKWebViewTransportContracts "${webtest_source_dir}" 60)

    add_library(wkwebview_class_module SHARED "${webtest_source_dir}/WKWebViewClassModule.cpp")
    add_executable(wkwebview_class_images "${webtest_source_dir}/WKWebViewClassImages.cpp")
    foreach(target wkwebview_class_module wkwebview_class_images)
        target_link_libraries(${target} PRIVATE WebFront WebFront_warnings WebFront_options)
        # Force distinct inline caches in both Mach-O images, rather than linker coalescing.
        set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    endforeach()
    add_dependencies(wkwebview_class_images wkwebview_class_module)
    add_dependencies(webtest wkwebview_class_images)
    foreach(order executable-first module-first)
        add_test(NAME WKWebViewClassImages-${order}
            COMMAND wkwebview_class_images $<TARGET_FILE:wkwebview_class_module> ${order})
        webfront_set_browser_test_properties("WKWebViewClassImages-${order}" "${webtest_source_dir}" 90)
    endforeach()
    # Stable agent bundles avoid one anonymous executable tile per test process in the Dock.
    # The path is normalized: a second spelling of the template would be a duplicate Ninja output.
    get_filename_component(_wk_plist "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../test/cmake/WKWebViewTestInfo.plist.in" ABSOLUTE)
    foreach(target webtest frontend_lifecycle wkwebview_lifecycle wkwebview_class_images wkwebview_transport_benchmark)
        string(REPLACE "_" "-" identifier "${target}")
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.webfront.tests.${identifier}"
            MACOSX_BUNDLE_INFO_PLIST "${_wk_plist}")
    endforeach()
endfunction()
