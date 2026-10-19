/** @brief Module-free WebFront diagnostic facade (ADR-003 Decision 3). */
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
