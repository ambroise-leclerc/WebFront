// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-18
//
// WebFront — tests/unit
// Role: Scope fixture for tasks whose mock scheduler and cancellation complete inline.
// Thread: bridge-io | Alloc: zero-hot-path
// LLM: test helper

#pragma once
#include <webfront/networking/NetworkingMock.hpp>

namespace webfront::testing {
class InlineTaskScope : public networking::TaskScope {
    networking::IoContextMock context;

public:
    ~InlineTaskScope() {
        drainUntilJoined(context);
    }
};
}  // namespace webfront::testing
