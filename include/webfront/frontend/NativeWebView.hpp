#pragma once

#if defined(_WIN32)
    #include "WebView2.hpp"
#elif defined(__APPLE__)
    #include "WKWebView.hpp"
#elif defined(__linux__)
    #include "WebKitGTK.hpp"
#endif

// Other platforms have no native alias; their applications can select another frontend.
namespace webfront::frontend {

#if defined(_WIN32)
/** @brief Opt-in Windows web view; availability and options are those of WebView2Frontend. */
using NativeWebViewFrontend = WebView2Frontend;
#elif defined(__APPLE__)
/** @brief Opt-in macOS web view; availability and options are those of WKWebViewFrontend. */
using NativeWebViewFrontend = WKWebViewFrontend;
#elif defined(__linux__)
/** @brief Opt-in Linux web view; availability and options are those of WebKitGTKFrontend. */
using NativeWebViewFrontend = WebKitGTKFrontend;
#endif

}  // namespace webfront::frontend
