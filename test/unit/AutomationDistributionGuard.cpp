// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-22
//
// WebFront — tests/unit
// Role: Must not compile: automation is absent from a distribution build (ADR-025).
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: compiled only by the AutomationAbsentFromDistribution test; a successful compile is the failure

// Compiled only by the AutomationAbsentFromDistribution test, with WEBFRONT_DISTRIBUTION=1: it must not compile (ADR-025).

#include <webfront/automation/Enable.hpp>
