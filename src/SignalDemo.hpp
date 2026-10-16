#pragma once

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace demo {

inline void validateSignalParameter(double value, double minimum, double maximum) {
    const bool finite  = std::isfinite(value);
    const bool inRange = value >= minimum && value <= maximum;
    if (!finite || !inRange)
        throw std::invalid_argument("Signal parameters are outside the supported range");
}

// One second of deterministic sample data for the React example, not a real acquisition device.
inline std::vector<double> generateSignal(double frequency, double amplitude, double harmonics) {
    validateSignalParameter(frequency, 1, 12);
    validateSignalParameter(amplitude, 0.1, 1.5);
    validateSignalParameter(harmonics, 0, 1);

    std::vector<double> samples(256);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto phase = 2 * std::numbers::pi * frequency * static_cast<double>(index) / 256;
        samples[index]   = amplitude * (std::sin(phase) + harmonics * std::sin(3 * phase) / 3);
    }
    return samples;
}

}  // namespace demo
