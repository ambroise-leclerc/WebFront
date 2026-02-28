/// @date 28/02/2026
/// @author Ambroise Leclerc
/// @brief Configurable buffer sizes policy for WebFront internal buffers
#pragma once

#include <concepts>
#include <cstddef>
#include <stdexcept>

namespace webfront::http {

/// Exception thrown when an internal buffer overflows during encoding
class BufferOverflowException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Concept defining requirements for a valid buffers policy
template<typename T>
concept BuffersPolicyType = requires {
    { T::receptionBufferSize } -> std::convertible_to<size_t>;
    { T::emissionBufferSize } -> std::convertible_to<size_t>;
    { T::functionCallBufferSize } -> std::convertible_to<size_t>;
};

/// Default buffer sizes as specified in issue #40
struct DefaultBuffersPolicy {
    static constexpr size_t receptionBufferSize    = 8192;
    static constexpr size_t emissionBufferSize     = 8192;
    static constexpr size_t functionCallBufferSize = 32768;
};

/// Custom policy template for user-defined buffer sizes
template<size_t ReceptionSize = 8192, size_t EmissionSize = 8192, size_t FunctionCallSize = 32768>
struct CustomBuffersPolicy {
    static constexpr size_t receptionBufferSize    = ReceptionSize;
    static constexpr size_t emissionBufferSize     = EmissionSize;
    static constexpr size_t functionCallBufferSize = FunctionCallSize;
};

static_assert(BuffersPolicyType<DefaultBuffersPolicy>);

} // namespace webfront::http
