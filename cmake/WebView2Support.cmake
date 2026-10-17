# The SDK is independent of the Evergreen runtime installed on the end user's machine.
if(NOT WIN32)
    message(FATAL_ERROR "WEBFRONT_EMBED_WEBVIEW2 is supported only on Windows")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "WEBFRONT_EMBED_WEBVIEW2 requires an MSVC-compatible compiler (MSVC or clang-cl); MinGW is not supported")
endif()

# The compiler target can differ from the host, particularly with Ninja on ARM64.
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _webview2_target)
if(NOT _webview2_target)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _webview2_target)
    if(_webview2_target STREQUAL "win32")
        set(_webview2_target x86)
    endif()
endif()
if(NOT _webview2_target)
    # clang-cl may leave the architecture ID empty. Probe its target macros, never host CPU or pointer size.
    include(CheckCXXSourceCompiles)
    foreach(_arch ARM64EC ARM64 X64 IX86)
        check_cxx_source_compiles("#ifndef _M_${_arch}\n#error different target\n#endif\nint main() { return 0; }"
            _webview2_has_${_arch})
    endforeach()
    if(_webview2_has_ARM64EC)
        set(_webview2_target arm64ec)
    elseif(_webview2_has_ARM64)
        set(_webview2_target arm64)
    elseif(_webview2_has_X64)
        set(_webview2_target x64)
    elseif(_webview2_has_IX86)
        set(_webview2_target x86)
    endif()
endif()
if(_webview2_target STREQUAL "arm64")
    set(_webview2_arch arm64)
elseif(_webview2_target STREQUAL "x64")
    set(_webview2_arch x64)
elseif(_webview2_target STREQUAL "x86")
    set(_webview2_arch x86)
else()
    message(FATAL_ERROR "WebView2 SDK loader does not support compiler target '${_webview2_target}'")
endif()

CPMAddPackage(
    NAME WebView2SDK
    VERSION 1.0.3537.50
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3537.50/microsoft.web.webview2.1.0.3537.50.nupkg
    URL_HASH SHA256=5ea526bbd728adda0da4d31219267e96460494a427e4894c4e09d9f320f4b9aa
    DOWNLOAD_ONLY YES
)

# Static loader: consumers need no WebView2Loader.dll deployment step.
target_include_directories(WebFront SYSTEM INTERFACE "${WebView2SDK_SOURCE_DIR}/build/native/include")
target_compile_definitions(WebFront INTERFACE WEBFRONT_EMBED_WEBVIEW2)
target_link_libraries(WebFront INTERFACE
    "${WebView2SDK_SOURCE_DIR}/build/native/${_webview2_arch}/WebView2LoaderStatic.lib"
    ole32 oleaut32 user32 shell32 shlwapi version)

# Additional lifecycle coverage for this engine, alongside the shared Jasmine runner.
function(webfront_add_webview2_tests)
    add_executable(webview2_lifecycle WebView2Lifecycle.cpp WebView2HeaderConsumer.cpp)
    target_link_libraries(webview2_lifecycle PRIVATE WebFront WebFront_warnings WebFront_options)
    foreach(mode no-bridge early-close rejected-ready navigation-failure mta missing-runtime
                 closed-startup-timeout failed-startup-timeout late-navigation-failure path-encoding runtime-ready)
        add_test(NAME WebView2Lifecycle-${mode} COMMAND webview2_lifecycle ${mode})
        set_tests_properties(WebView2Lifecycle-${mode} PROPERTIES
            LABELS "web-integration" TIMEOUT 45 RESOURCE_LOCK webfront_port_9002
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/webtest)
    endforeach()
endfunction()
