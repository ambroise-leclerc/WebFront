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

/// Largest WebSocket message received when a policy has no maxMessageSize.
inline constexpr size_t defaultMaxMessageSize = 64 * 1024 * 1024;

/// Largest identity response decoded from a gzip asset by default.
inline constexpr size_t defaultMaxDecodedAssetSize = 4 * 1024 * 1024;

/// Retained compressed keys, decoded data and entry storage per server.
inline constexpr size_t defaultMaxDecodedAssetCacheSize = 8 * 1024 * 1024;

/// Default buffer sizes as specified in issue #40
struct DefaultBuffersPolicy {
    static constexpr size_t receptionBufferSize    = 8192;
    static constexpr size_t emissionBufferSize     = 8192;
    static constexpr size_t functionCallBufferSize = 32768;
    static constexpr size_t maxMessageSize         = defaultMaxMessageSize;
    static constexpr size_t maxDecodedAssetSize    = defaultMaxDecodedAssetSize;
    static constexpr size_t maxDecodedAssetCacheSize = defaultMaxDecodedAssetCacheSize;
    /// Optional gzip worker limits; legacy policies use these defaults too.
    static constexpr size_t maxGzipDecodeJobs = 4;
    static constexpr size_t maxGzipDecodeWaiters = 16;
    static constexpr size_t maxGzipDecodeBytes = 64 * 1024 * 1024;
    static constexpr size_t maxGzipEncodedAssetSize = 4 * 1024 * 1024;
};

/// Custom policy template for user-defined buffer sizes
template <size_t ReceptionSize       = 8192,
          size_t EmissionSize        = 8192,
          size_t FunctionCallSize    = 32768,
          size_t MaxMessageSize      = defaultMaxMessageSize,
          size_t MaxDecodedAssetSize = defaultMaxDecodedAssetSize,
          size_t MaxDecodedAssetCacheSize = defaultMaxDecodedAssetCacheSize>
struct CustomBuffersPolicy {
    static constexpr size_t receptionBufferSize    = ReceptionSize;
    static constexpr size_t emissionBufferSize     = EmissionSize;
    static constexpr size_t functionCallBufferSize = FunctionCallSize;
    static constexpr size_t maxMessageSize         = MaxMessageSize;
    static constexpr size_t maxDecodedAssetSize    = MaxDecodedAssetSize;
    static constexpr size_t maxDecodedAssetCacheSize = MaxDecodedAssetCacheSize;
};

/// The policy's optional maxMessageSize: a WebSocket closes the connection with
/// std::errc::message_size as soon as a frame header announces a message larger than this, in one
/// frame or adding up its fragments, before buffering its payload. Control frames (close, ping,
/// pong) have their own 125-byte protocol limit instead.
template<BuffersPolicyType Policy>
constexpr size_t maxMessageSize() {
    if constexpr (requires { { Policy::maxMessageSize } -> std::convertible_to<size_t>; })
        return Policy::maxMessageSize;
    else
        return defaultMaxMessageSize;
}

/** @brief Optional gzip fallback budget; legacy policies retain the default. Zero disables nonempty decoded responses. */
template <BuffersPolicyType Policy>
constexpr size_t maxDecodedAssetSize() {
    if constexpr (requires {
                      { Policy::maxDecodedAssetSize } -> std::convertible_to<size_t>;
                  })
        return Policy::maxDecodedAssetSize;
    else
        return defaultMaxDecodedAssetSize;
}

/** @brief Optional retained gzip cache budget. Zero disables retention; legacy policies use 8 MiB. */
template <BuffersPolicyType Policy>
constexpr size_t maxDecodedAssetCacheSize() {
    if constexpr (requires {
                      { Policy::maxDecodedAssetCacheSize } -> std::convertible_to<size_t>;
                  })
        return Policy::maxDecodedAssetCacheSize;
    else
        return defaultMaxDecodedAssetCacheSize;
}

static_assert(BuffersPolicyType<DefaultBuffersPolicy>);

} // namespace webfront::http
