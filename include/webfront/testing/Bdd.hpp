// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-14
//
// WebFront — testing
// Role: The whole webfront::bdd framework in one include: the core and every optional part.
// Thread: worker-safe | Alloc: unrestricted
// LLM: public maintained interface (docs/BDD.md); umbrella only, the code lives in webfront/testing/bdd/

// The whole behaviour-driven test framework (docs/BDD.md). A test file that wants to compile faster includes
// <webfront/testing/bdd/Core.hpp> and only the optional parts it uses:
// - bdd/Matchers.hpp: string matchers (contains, endsWith, equals) and check(text, matcher);
// - bdd/Approx.hpp: approx(), floating-point comparison within a tolerance;
// - bdd/Eventually.hpp: waits for an asynchronous state (requireEventually, requireReady...), with <chrono> and <thread>;
// - bdd/Runner.hpp: run(), for the one file that defines main().

#pragma once

#include <webfront/testing/bdd/Approx.hpp>
#include <webfront/testing/bdd/Core.hpp>
#include <webfront/testing/bdd/Eventually.hpp>
#include <webfront/testing/bdd/Matchers.hpp>
#include <webfront/testing/bdd/Runner.hpp>
