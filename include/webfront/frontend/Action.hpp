// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-16
//
// WebFront — frontend
// Role: Whether the server keeps running or stops once a frontend's open() returns.
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: tiny enum; part of the FrontendType contract in Frontend.hpp

#pragma once

namespace webfront::frontend {

enum class Action { keepServerRunning, closeServerAfterOpen };

}  // namespace webfront::frontend
