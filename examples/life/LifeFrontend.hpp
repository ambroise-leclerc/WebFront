#pragma once

#include <webfront/DeveloperApp.hpp>
#include <webfront/WebFront.hpp>

namespace webfront::demo {

// examples/CMakeLists.txt defines WEBFRONT_LIFE_NATIVE_WINDOW when this platform's native engine is built.
// Otherwise the Life examples open the system browser, which needs no extra dependency.
#if defined(WEBFRONT_LIFE_NATIVE_WINDOW)
using LifeFrontend                        = frontend::NativeWebViewFrontend;
inline constexpr bool lifeNativeWindow    = true;
#else
using LifeFrontend                        = frontend::DefaultBrowserFrontend;
inline constexpr bool lifeNativeWindow    = false;
#endif

using LifeApp = BasicDeveloperApp<LifeFrontend>;

template <fs::Provider... Assets>
using LifeAppWithAssets = BasicWF<NetProvider, fs::DefaultFSWith<Assets...>, http::DefaultBuffersPolicy, LifeFrontend>;

inline constexpr const char* browserNotice =
    "No native window engine was built: the default browser is opened instead. For a native window, install the "
    "engine for your platform and run cmake again (see the README).";

}  // namespace webfront::demo
