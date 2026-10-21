#include "Life.hpp"

#include <iostream>

int main() {
    webfront::demo::LifeGrid cells{0b111};
    for (int generation = 0; generation < 4; ++generation) {
        std::cout << "Generation " << generation << ": " << cells.count() << " living cells\n";
        cells = webfront::demo::nextLife(cells);
    }
}
