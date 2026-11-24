// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-01
//
// WebFront — tooling
// Role: Module-free WebFront diagnostic facade over mddlog (ADR-003 decision 3).
// Thread: thread-safe | Alloc: heap-init
// LLM: facade only; the module-consuming TU is cmake/mddlog/Logger.cpp

// Module-free WebFront diagnostic facade (ADR-003 Decision 3).

#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

// The declaration-only API requires the standard headers above.
// clang-format off
#include "LoggerApi.hpp"  // IWYU pragma: export
// clang-format on
