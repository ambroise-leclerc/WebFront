// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-16
//
// WebFront — networking
// Role: Every stdnet and stdexec header WebFront uses, included before CEF's own headers.
// Thread: worker-safe | Alloc: zero-hot-path
// LLM: include-order shim; add any new stdnet or stdexec header here, not elsewhere

// Every stdnet and stdexec header WebFront uses.
//
// CEF defines macros (NO_UNIQUE_ADDRESS, ALWAYS_INLINE, NOINLINE) that are also the labels stdexec pastes
// onto STDEXEC_ATTRIBUTE_CASE_: a stdexec header included after CEF's headers does not compile.
// frontend/CEF.hpp includes this header before CEF's, so a stdnet or stdexec header used anywhere in
// WebFront must be added here.

#pragma once

#include <network>
