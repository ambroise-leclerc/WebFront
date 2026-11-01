/// @brief Frontend-neutral capture of the web view's content as a PNG image, for tests and CI.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace webfront::frontend {

/// The PNG bytes of the web content, or a diagnostic.
using CaptureResult = std::expected<std::vector<std::uint8_t>, std::string>;

/// Produces one PNG of the live window. It may be called from any thread and blocks for at most the given time.
using CaptureProvider = std::function<CaptureResult(std::chrono::milliseconds)>;

namespace detail {
struct CaptureRegistry {
    std::mutex                       mutex;
    std::shared_ptr<CaptureProvider> provider;
};
inline CaptureRegistry& captureRegistry() {
    static CaptureRegistry registry;
    return registry;
}
}  // namespace detail

/**
 * @brief Makes a window capturable while it lives. A frontend holds one for its open window.
 * One window is supported per process: a later registration replaces the earlier one, and a stale
 * registration's destruction never removes its successor.
 */
class CaptureRegistration {
public:
    CaptureRegistration() = default;
    explicit CaptureRegistration(CaptureProvider provider) : owned(std::make_shared<CaptureProvider>(std::move(provider))) {
        auto& registry = detail::captureRegistry();
        std::scoped_lock lock{registry.mutex};
        registry.provider = owned;
    }
    CaptureRegistration(const CaptureRegistration&)            = delete;
    CaptureRegistration& operator=(const CaptureRegistration&) = delete;
    CaptureRegistration(CaptureRegistration&& other) noexcept : owned(std::exchange(other.owned, nullptr)) {}
    CaptureRegistration& operator=(CaptureRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            owned = std::exchange(other.owned, nullptr);
        }
        return *this;
    }
    ~CaptureRegistration() {
        reset();
    }

private:
    void reset() noexcept {
        if (!owned)
            return;
        auto& registry = detail::captureRegistry();
        {
            std::scoped_lock lock{registry.mutex};
            if (registry.provider == owned)
                registry.provider.reset();
        }
        owned.reset();
    }

    std::shared_ptr<CaptureProvider> owned;
};

/**
 * @brief Captures the web content of the open window as a PNG (not the native decorations).
 * Callable from any thread except the one that runs the window's event loop. Fails with a diagnostic when no
 * window is open, the frontend cannot capture, or the window does not answer within the timeout.
 */
inline CaptureResult capturePng(std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    std::shared_ptr<CaptureProvider> provider;
    {
        auto& registry = detail::captureRegistry();
        std::scoped_lock lock{registry.mutex};
        provider = registry.provider;
    }
    if (!provider)
        return std::unexpected("no window that can be captured is open (the frontend may not support capture)");
    return (*provider)(timeout);
}

}  // namespace webfront::frontend
