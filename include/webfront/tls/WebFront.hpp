// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-21
//
// WebFront — tls
// Role: Convenience alias of WebFront using the TLS provider.
// Thread: ui-loop | Alloc: zero-hot-path
// LLM: alias only; see the TLS documentation before use

#pragma once
#include "../WebFront.hpp"
#include "Server.hpp"

namespace webfront::tls {
template <frontend::FrontendType Frontend = frontend::DefaultFrontend, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
using WebFront = BasicWF<Server, fs::DefaultFS, Policy, Frontend>;
}  // namespace webfront::tls
