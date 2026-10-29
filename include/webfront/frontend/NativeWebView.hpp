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

/**
 * @brief True when this platform's native web view engine is compiled in (WEBFRONT_EMBED_WEBKITGTK on Linux,
 * WEBFRONT_EMBED_WEBVIEW2 on Windows, WEBFRONT_EMBED_WKWEBVIEW on macOS).
 *
 * It says nothing about the session: a native window can still fail at run time, for instance without a display.
 * NativeOrBrowserFrontend handles that case.
 */
#if (defined(_WIN32) && defined(WEBFRONT_EMBED_WEBVIEW2)) || (defined(__APPLE__) && defined(WEBFRONT_EMBED_WKWEBVIEW)) \
    || (defined(__linux__) && defined(WEBFRONT_EMBED_WEBKITGTK))
inline constexpr bool nativeWebViewAvailable = true;
#else
inline constexpr bool nativeWebViewAvailable = false;
#endif

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
