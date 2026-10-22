#include "../fixtures/SignalDemo.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <numeric>

SCENARIO("The React signal demo generates a one-second waveform in C++") {
    GIVEN("A four-Hertz sine wave at 0.8 volts") {
        const auto samples = demo::generateSignal(4, 0.8, 0);
        THEN("The buffer has 256 samples and the expected phase and RMS") {
            REQUIRE(samples.size() == 256);
            REQUIRE(samples[0] == Catch::Approx(0).margin(1e-12));
            REQUIRE(samples[16] == Catch::Approx(0.8));
            REQUIRE(samples[48] == Catch::Approx(-0.8));
            const auto energy = std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
            REQUIRE(std::sqrt(energy / 256) == Catch::Approx(0.8 / std::sqrt(2.0)));
        }
        WHEN("A third harmonic is mixed in") {
            const auto mixed = demo::generateSignal(4, 0.8, 0.6);
            THEN("The samples contain that harmonic") {
                REQUIRE(mixed[16] == Catch::Approx(0.64));
                REQUIRE(mixed != samples);
            }
        }
    }
}

SCENARIO("The React signal demo rejects invalid parameters") {
    GIVEN("Invalid frequencies, amplitudes or harmonic mixes") {
        for (const auto frequency : {0.0, 13.0, std::numeric_limits<double>::quiet_NaN()})
            REQUIRE_THROWS_AS(demo::generateSignal(frequency, 0.8, 0.3), std::invalid_argument);
        for (const auto amplitude : {0.0, 1.6, std::numeric_limits<double>::infinity()})
            REQUIRE_THROWS_AS(demo::generateSignal(4, amplitude, 0.3), std::invalid_argument);
        for (const auto harmonics : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()})
            REQUIRE_THROWS_AS(demo::generateSignal(4, 0.8, harmonics), std::invalid_argument);
        THEN("The supported boundary values still produce finite samples") {
            for (const auto sample : demo::generateSignal(12, 1.5, 1))
                REQUIRE(std::isfinite(sample));
            REQUIRE(demo::generateSignal(1, 0.1, 0).size() == 256);
        }
    }
}
