// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-25
//
// WebFront — examples
// Role: Selects the frontend of the Game of Life examples.
// Thread: ui-loop | Alloc: zero-hot-path
// LLM: alias only; the examples never select CEF

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
