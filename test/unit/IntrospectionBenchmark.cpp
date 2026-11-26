// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-26
//
// WebFront — tests/unit
// Role: Benchmark of what the inspectors' registry adds to a call: the counters of a cppFunction and of a call to JavaScript.
// Thread: worker-safe | Alloc: unrestricted
// LLM: measures only; never fails on timing

// Manual Release benchmark (ADR-027, section 7): the cost per call of the counters when no inspector is open, which is
// what every application pays, and of the timing while one is open.

#include <webfront/Introspection.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>

namespace {
using namespace webfront;

constexpr std::uint64_t iterations = 20'000'000;

/// Nanoseconds per iteration of `body`, the best of five runs.
template <typename Body>
double measure(Body body) {
    double best = 1e300;
    for (int run = 0; run < 5; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < iterations; ++i)
            body(i);
        const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        best               = std::min(best, elapsed / static_cast<double>(iterations));
    }
    return best;
}

volatile std::uint64_t sink = 0;
}  // namespace

int main() {
    inspect::FunctionRegistry      registry;
    const auto                     record = registry.add("benchmark", "() => Promise<void>", false, false, std::source_location::current());
    inspect::FunctionRecord* const none   = nullptr;

    const double baseline = measure([&](std::uint64_t i) {
        const inspect::CallMeasure call{none};
        sink = i;
    });
    const double counted  = measure([&](std::uint64_t i) {
        const inspect::CallMeasure call{record.get()};
        sink = i;
    });
    registry.observe(std::chrono::hours{1});
    const double timed = measure([&](std::uint64_t i) {
        const inspect::CallMeasure call{record.get()};
        sink = i;
    });

    inspect::JsCallRegistry calls;
    static_cast<void>(calls.counter("draw"));
    const double jsCall = measure([&](std::uint64_t i) {
        calls.counter("draw")->called();
        sink = i;
    });

    std::printf("call without a record (distribution build, inspectors' own functions): %7.2f ns\n", baseline);
    std::printf("call counted, no inspector open (one counter, one flag read):           %7.2f ns  (+%.2f ns)\n", counted, counted - baseline);
    std::printf("call counted and timed, an inspector open (two clock reads):            %7.2f ns  (+%.2f ns)\n", timed, timed - baseline);
    std::printf("call to JavaScript counted by name, developer environment mounted:      %7.2f ns\n", jsCall);
    return 0;
}
