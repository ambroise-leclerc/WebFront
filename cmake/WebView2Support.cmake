# The SDK is independent of the Evergreen runtime installed on the end user's machine.
if(NOT WIN32)
    message(FATAL_ERROR "WEBFRONT_EMBED_WEBVIEW2 is supported only on Windows")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "WEBFRONT_EMBED_WEBVIEW2 requires an MSVC-compatible compiler (MSVC or clang-cl); MinGW is not supported")
endif()

CPMAddPackage(
    NAME WebView2SDK
    VERSION 1.0.3537.50
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3537.50/microsoft.web.webview2.1.0.3537.50.nupkg
    URL_HASH SHA256=5ea526bbd728adda0da4d31219267e96460494a427e4894c4e09d9f320f4b9aa
    DOWNLOAD_ONLY YES
)

# The compiler target can differ from the host, particularly with Ninja on ARM64.
string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" _webview2_target)
if(_webview2_target STREQUAL "arm64")
    set(_webview2_arch arm64)
elseif(_webview2_target STREQUAL "x64")
    set(_webview2_arch x64)
elseif(_webview2_target STREQUAL "x86")
    set(_webview2_arch x86)
else()
    message(FATAL_ERROR "WebView2 SDK loader does not support compiler target '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'")
endif()

# Static loader: consumers need no WebView2Loader.dll deployment step.
target_include_directories(WebFront SYSTEM INTERFACE "${WebView2SDK_SOURCE_DIR}/build/native/include")
target_compile_definitions(WebFront INTERFACE WEBFRONT_EMBED_WEBVIEW2)
target_link_libraries(WebFront INTERFACE
    "${WebView2SDK_SOURCE_DIR}/build/native/${_webview2_arch}/WebView2LoaderStatic.lib"
    ole32 oleaut32 user32 shell32 shlwapi version)
