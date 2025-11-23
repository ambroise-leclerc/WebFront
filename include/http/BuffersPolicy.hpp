/// @file BuffersPolicy.hpp
/// @brief Configurable buffer sizes policy for WebFront
/// @author Ambroise Leclerc
/// @version 1.0
/// @date 2025-11-23

#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace webfront::http {

/// @brief Exception thrown when a buffer overflow is detected
class BufferOverflowException : public std::runtime_error {
public:
    explicit BufferOverflowException(const std::string& message)
        : std::runtime_error(message) {}

    explicit BufferOverflowException(const char* message)
        : std::runtime_error(message) {}
};

/// @brief Concept defining requirements for a valid BuffersPolicy
template<typename T>
concept BuffersPolicyType = requires {
    { T::receptionBufferSize } -> std::convertible_to<size_t>;
    { T::emissionBufferSize } -> std::convertible_to<size_t>;
    { T::functionCallBufferSize } -> std::convertible_to<size_t>;
};

/// @brief Default buffers policy with configurable buffer sizes
/// @details This policy defines the default sizes for internal buffers used by WebFront:
///          - receptionBufferSize: Size of buffers used for receiving HTTP/WebSocket data
///          - emissionBufferSize: Size of buffers used for sending HTTP/WebSocket data
///          - functionCallBufferSize: Size of buffers used for encoding function call parameters
struct DefaultBuffersPolicy {
    /// @brief Size of reception buffers (default: 8192 bytes)
    static constexpr size_t receptionBufferSize = 8192;

    /// @brief Size of emission buffers (default: 8192 bytes)
    static constexpr size_t emissionBufferSize = 8192;

    /// @brief Size of function call encoding buffers (default: 32768 bytes)
    static constexpr size_t functionCallBufferSize = 32768;
};

/// @brief Custom buffers policy template for user-defined buffer sizes
/// @tparam ReceptionSize Size of reception buffers in bytes
/// @tparam EmissionSize Size of emission buffers in bytes
/// @tparam FunctionCallSize Size of function call encoding buffers in bytes
template<size_t ReceptionSize = 8192, size_t EmissionSize = 8192, size_t FunctionCallSize = 32768>
struct CustomBuffersPolicy {
    static constexpr size_t receptionBufferSize = ReceptionSize;
    static constexpr size_t emissionBufferSize = EmissionSize;
    static constexpr size_t functionCallBufferSize = FunctionCallSize;
};

// Ensure DefaultBuffersPolicy satisfies the concept
static_assert(BuffersPolicyType<DefaultBuffersPolicy>, "DefaultBuffersPolicy must satisfy BuffersPolicyType concept");

} // namespace webfront::http
