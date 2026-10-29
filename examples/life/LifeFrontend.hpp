#pragma once

#include <webfront/DeveloperApp.hpp>
#include <webfront/WebFront.hpp>

namespace webfront::demo {

// A native window when this platform's engine is built and a session exists, the system browser otherwise
// (no engine, SSH, container, CI). The reason for a fallback is printed once by the frontend.
using LifeFrontend = frontend::NativeOrBrowserFrontend;

using LifeApp = BasicDeveloperApp<LifeFrontend>;

template <fs::Provider... Assets>
using LifeAppWithAssets = BasicWF<NetProvider, fs::DefaultFSWith<Assets...>, http::DefaultBuffersPolicy, LifeFrontend>;

}  // namespace webfront::demo
