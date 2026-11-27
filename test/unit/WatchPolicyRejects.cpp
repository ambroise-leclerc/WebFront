// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — tests/unit
// Role: Buffers policies too small for the inspectors: each REJECT_* case must fail to compile on its assertion (ADR-027, section 3).
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: compiled by CTest with -fsyntax-only (WatchPolicyRejects.*); without a REJECT_* macro the file compiles

#include <webfront/WatchValues.hpp>

using namespace webfront;

#if defined(REJECT_SMALL_BUFFER)
// The encoding buffer cannot hold a page of one node.
using Policy = http::CustomBuffersPolicy<8192, 8192, 2048>;
#elif defined(REJECT_SMALL_MESSAGE)
// The buffer is large enough, but a message is not: only maxMessageSize is too small.
using Policy = http::CustomBuffersPolicy<8192, 8192, 32768, 4000>;
#elif defined(REJECT_SMALL_WRITE)
// A message so small that a write request has less than 1 KiB left for its text.
using Policy = http::CustomBuffersPolicy<8192, 8192, 32768, 2000>;
#else
using Policy = http::DefaultBuffersPolicy;
#endif

/// What the developer environment does when it registers webfront.inspector.* (DeveloperApp.hpp).
static constexpr inspect::Limits limits = inspect::limitsOf<Policy>();

int main() {
    return limits.responseBudget >= inspect::minPageBytes ? 0 : 1;
}
