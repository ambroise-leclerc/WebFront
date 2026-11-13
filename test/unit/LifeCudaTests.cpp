#include "../../examples/life/cuda/LifeEngine.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using webfront::demo::lifecuda::Backend;
using webfront::demo::lifecuda::Field;
using webfront::demo::lifecuda::fieldSide;
using webfront::demo::lifecuda::Pattern;

namespace {
// The rules as the README states them, one cell at a time: the reference the word-parallel kernel must match.
Field nextByCells(const Field& field) {
    const auto side = field.side();
    Field      next{side};
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            int neighbors = 0;
            for (const auto dy : {side - 1, 0u, 1u})
                for (const auto dx : {side - 1, 0u, 1u})
                    if (dx || dy)
                        neighbors += field.alive((x + dx) % side, (y + dy) % side);
            next.set(x, y, neighbors == 3 || (field.alive(x, y) && neighbors == 2));
        }
    return next;
}
}  // namespace

SCENARIO("The word-parallel Life kernel follows B3/S23 on a torus") {
    GIVEN("random fields whose rows are one, two and three words wide") {
        for (const std::uint32_t side : {64u, 128u, 192u}) {
            Field field{side};
            field.seed(Pattern::soup, side);
            THEN("every generation equals the cell-by-cell reference, side " + std::to_string(side)) {
                for (int generation = 0; generation < 12; ++generation) {
                    const auto expected = nextByCells(field);
                    field.advance(Backend::cpu, 1);
                    REQUIRE(field == expected);
                }
                REQUIRE(field.generation() == 12);
            }
        }
    }
    GIVEN("a glider crossing a word boundary and both edges") {
        Field field{64};
        for (const auto& [x, y] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{{62, 61}, {63, 62}, {61, 63}, {62, 63}, {63, 63}})
            field.set(x, y);
        WHEN("it has moved one cell diagonally four times") {
            field.advance(Backend::cpu, 16);
            THEN("it has wrapped around the corner of the torus") {
                Field expected{64};
                for (const auto& [x, y] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{{2, 1}, {3, 2}, {1, 3}, {2, 3}, {3, 3}})
                    expected.set(x, y);
                REQUIRE(field == expected);
                REQUIRE(field.population() == 5);
            }
        }
    }
    GIVEN("a field whose side is not a multiple of 64") {
        REQUIRE_THROWS_AS(Field{100}, std::invalid_argument);
        REQUIRE_THROWS_AS(Field{0}, std::invalid_argument);
    }
}

SCENARIO("A Life field is packed for the bridge like a std::bitset") {
    GIVEN("the demo's field with three cells") {
        Field field;
        field.set(0, 0);
        field.set(9, 0);
        field.set(fieldSide - 1, fieldSide - 1);
        WHEN("it is packed") {
            const auto bytes = field.packed();
            THEN("cell i is bit i % 8 of byte i / 8") {
                REQUIRE(bytes.size() == std::size_t{fieldSide} * fieldSide / 8);
                REQUIRE(bytes[0] == 0b0000'0001);
                REQUIRE(bytes[1] == 0b0000'0010);
                REQUIRE(bytes.back() == 0b1000'0000);
                REQUIRE(field.population() == 3);
            }
        }
    }
    GIVEN("the starting patterns") {
        Field first, second;
        first.seed(Pattern::soup, 7);
        second.seed(Pattern::soup, 7);
        THEN("a seed always gives the same soup, and another seed another one") {
            REQUIRE(first == second);
            second.seed(Pattern::soup, 8);
            REQUIRE_FALSE(first == second);
        }
        THEN("seeding resets the generation and the named patterns have their cells") {
            first.advance(Backend::cpu, 2);
            first.seed(Pattern::acorn);
            REQUIRE(first.generation() == 0);
            REQUIRE(first.population() == 7);
            first.seed(Pattern::gliders);
            REQUIRE(first.population() == 5u * (fieldSide / 16) * (fieldSide / 16));
            first.seed(Pattern::empty);
            REQUIRE(first.population() == 0);
            REQUIRE_THROWS_AS(first.seed(static_cast<Pattern>(99)), std::out_of_range);
        }
    }
}

SCENARIO("The CUDA backend of the Life field computes what the CPU computes") {
    namespace gpu = webfront::demo::lifecuda::gpu;
    GIVEN("an unknown backend") {
        Field field{64};
        REQUIRE_THROWS_AS(field.advance(static_cast<Backend>(2), 1), std::out_of_range);
        REQUIRE(field.generation() == 0);
    }
    if (!gpu::available()) {
        GIVEN("a build or a machine without CUDA") {
            Field field{64};
            field.seed(Pattern::soup);
            const auto before = field;
            THEN("asking for it fails instead of computing on the CPU, and leaves the field unchanged") {
                REQUIRE(webfront::demo::lifecuda::cudaDevice().empty());
                REQUIRE_THROWS_AS(field.advance(Backend::cuda, 1), std::runtime_error);
                REQUIRE(field == before);
                REQUIRE(field.generation() == 0);
            }
        }
        return;
    }
    GIVEN("the same soup on both backends") {
        REQUIRE_FALSE(webfront::demo::lifecuda::cudaDevice().empty());
        for (const std::uint32_t side : {64u, 192u, fieldSide}) {
            Field cpu{side}, cuda{side};
            cpu.seed(Pattern::soup, side);
            cuda.seed(Pattern::soup, side);
            THEN("they agree after one generation and after many in one call, side " + std::to_string(side)) {
                for (const std::uint32_t generations : {1u, 2u, 61u}) {
                    cpu.advance(Backend::cpu, generations);
                    cuda.advance(Backend::cuda, generations);
                    REQUIRE(cuda == cpu);
                }
                REQUIRE(cuda.generation() == 64);
            }
        }
        gpu::release();
    }
}
