// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-20
//
// WebFront — examples
// Role: Console stage of the Game of Life walkthrough.
// Thread: ui-loop | Alloc: unrestricted
// LLM: first stage of the README walkthrough

#include "Life.hpp"

#include <iostream>

int main() {
    webfront::demo::LifeGrid cells{0b111};
    for (int generation = 0; generation < 4; ++generation) {
        std::cout << "Generation " << generation << ": " << cells.count() << " living cells\n";
        cells = webfront::demo::nextLife(cells);
    }
}
