#include "../examples/life/Life.hpp"
#include <catch2/catch_test_macros.hpp>

using webfront::demo::LifeGrid;
using webfront::demo::nextLife;

SCENARIO("Life generations follow B3/S23 synchronously") {
    GIVEN("an empty grid and a stable block") {
        REQUIRE(nextLife({}).none());
        LifeGrid block;
        for (const auto i : {330, 331, 362, 363})
            block.set(static_cast<std::size_t>(i));
        REQUIRE(nextLife(block) == block);
    }
    GIVEN("a horizontal blinker") {
        LifeGrid horizontal, vertical;
        for (const auto i : {330, 331, 332})
            horizontal.set(static_cast<std::size_t>(i));
        for (const auto i : {299, 331, 363})
            vertical.set(static_cast<std::size_t>(i));
        WHEN("two generations are computed") {
            REQUIRE(nextLife(horizontal) == vertical);
            REQUIRE(nextLife(vertical) == horizontal);
            REQUIRE(horizontal.count() == 3);
        }
    }
    GIVEN("a glider") {
        LifeGrid grid, translated;
        for (const auto i : {1, 34, 64, 65, 66})
            grid.set(static_cast<std::size_t>(i));
        for (const auto i : {34, 67, 97, 98, 99})
            translated.set(static_cast<std::size_t>(i));
        for (int i = 0; i < 4; ++i)
            grid = nextLife(grid);
        REQUIRE(grid == translated);
    }
}

SCENARIO("Life wraps both axes without treating adjacent rows as neighbors") {
    LifeGrid horizontal, vertical;
    for (const auto i : {31, 0, 1})
        horizontal.set(static_cast<std::size_t>(i));
    for (const auto i : {992, 0, 32})
        vertical.set(static_cast<std::size_t>(i));
    REQUIRE(nextLife(horizontal) == vertical);
    REQUIRE(nextLife(vertical) == horizontal);
    LifeGrid isolated;
    for (const auto i : {30, 31, 34})
        isolated.set(static_cast<std::size_t>(i));
    REQUIRE(nextLife(isolated).none());
}
