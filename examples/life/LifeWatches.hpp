// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-27
//
// WebFront — examples
// Role: What the Life developer example shows in the inspectors (docs/WATCHES.md): the last grid computed and the number of generations.
// Thread: thread-safe | Alloc: heap-init
// LLM: example of app.watch; the visitor runs under the mutex, the handles are destroyed before it and outside any lock

#pragma once

#include "Life.hpp"
#include <webfront/WebFront.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace webfront::demo {

/**
 * The state the example keeps for the inspectors. The page owns the simulation and calls `life.step` for each generation;
 * C++ keeps the last grid it computed, as rows of text, and counts the generations.
 *
 * Here every access is on the server thread, which runs `life.step` and the inspectors' reads alike. The mutex is what an
 * application needs as soon as another thread computes: the watches below are written for that case.
 */
struct LifeObserved {
    std::mutex               mutex;
    std::vector<std::string> grid        = std::vector<std::string>(32, std::string(32, '.'));
    std::uint64_t            generations = 0;

    /// The next generation, kept for the inspectors.
    LifeGrid step(const LifeGrid& cells) {
        const auto       next = nextLife(cells);
        std::scoped_lock lock{mutex};
        for (std::size_t y = 0; y < grid.size(); ++y)
            for (std::size_t x = 0; x < grid[y].size(); ++x)
                grid[y][x] = next[y * 32 + x] ? '#' : '.';
        ++generations;
        return next;
    }
};

/**
 * Declares the watches of the example: `grid`, read-only, and `generations`, which the inspector can write.
 *
 * Each source is a visitor: WebFront calls it, it takes the lock and calls `visit` once with the object, so that a whole
 * operation of the inspector (a page, a pass of differences, a write) runs under the lock. A source that took the lock and
 * returned a reference would protect nothing: the lock would be released before the object is read.
 *
 * Keep the returned handles in a variable declared after `observed`: they are then destroyed before its mutex, and outside
 * any lock, which is what the destruction of a watch requires since it waits for an operation in progress.
 */
template <typename App>
[[nodiscard]] std::array<inspect::Watch, 2> watchLife(App& app, LifeObserved& observed) {
    return {app.watch("grid",
                      [&observed](inspect::Visit<const std::vector<std::string>> visit) {
                          std::scoped_lock lock{observed.mutex};
                          visit(observed.grid);
                      }),
            app.watch("generations", [&observed](inspect::Visit<std::uint64_t> visit) {
                std::scoped_lock lock{observed.mutex};
                visit(observed.generations);
            })};
}

}  // namespace webfront::demo
