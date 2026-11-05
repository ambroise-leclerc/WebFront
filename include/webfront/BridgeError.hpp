/// @brief The failure of a bridge call, with a code that lets the caller tell failures apart.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace webfront {

class BridgeError;
namespace detail {
/// Builds an error of the bridge itself, with a reserved code. For WebFront's own use: not part of the public API.
[[nodiscard]] BridgeError makeBridgeError(std::string_view code, const std::string& message);
}  // namespace detail

/// Codes the bridge itself reports. They all start with `webfront.`, a prefix an application cannot use.
namespace bridge_code {
/// An exception that carries no code of its own (a `std::exception` thrown by a callback, a JavaScript `Error` without `code`).
inline constexpr std::string_view generic = "webfront.error";
/// The called function does not exist, on the C++ or on the JavaScript side.
inline constexpr std::string_view notFound = "webfront.notFound";
/// A result, an argument or a message that cannot be decoded as what the caller expected.
inline constexpr std::string_view malformed = "webfront.malformed";
/// The browser connection closed before the call completed.
inline constexpr std::string_view disconnected = "webfront.disconnected";
/// The work that was to answer the call was stopped: the browser left, the application is shutting down or a sender was cancelled.
inline constexpr std::string_view cancelled = "webfront.cancelled";
}  // namespace bridge_code

/**
 * @brief An error with a code and a message, thrown by a callback to reject the JavaScript Promise with an `Error` whose
 * `code` is this code, and thrown by `std::future::get()` or `co_await` when a JavaScript Promise rejected with a `code`.
 *
 * A code is 1 to 64 characters among letters, digits and `. _ : -`. Codes starting with `webfront.` are reserved to the bridge
 * (see bridge_code): the constructor refuses them, so a failure reported by the bridge is never mistaken for an application's.
 * An exception that is not a BridgeError reaches JavaScript with the generic code and its message.
 */
class BridgeError : public std::runtime_error {
public:
    static constexpr std::size_t maxCodeSize = 64;
    static constexpr std::string_view reservedPrefix = "webfront.";

    /// @throws std::invalid_argument for an ill-formed or reserved code
    BridgeError(std::string errorCode, const std::string& message) : std::runtime_error(message), codeText(std::move(errorCode)) {
        if (!isValidCode(codeText))
            throw std::invalid_argument("A bridge error code is 1 to 64 letters, digits or . _ : -");
        if (isReserved(codeText))
            throw std::invalid_argument("Bridge error codes starting with 'webfront.' are reserved to WebFront");
    }

    [[nodiscard]] const std::string& code() const noexcept {
        return codeText;
    }

    [[nodiscard]] static bool isValidCode(std::string_view code) noexcept {
        if (code.empty() || code.size() > maxCodeSize)
            return false;
        for (const char c : code) {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
            if (!allowed)
                return false;
        }
        return true;
    }

    [[nodiscard]] static bool isReserved(std::string_view code) noexcept {
        return code.starts_with(reservedPrefix);
    }

private:
    friend BridgeError detail::makeBridgeError(std::string_view, const std::string&);
    struct Bridge {};
    BridgeError(Bridge, std::string errorCode, const std::string& message) : std::runtime_error(message), codeText(std::move(errorCode)) {}

    std::string codeText;
};

namespace detail {
/// A code that is ill-formed (a peer is not trusted) becomes the generic one.
[[nodiscard]] inline BridgeError makeBridgeError(std::string_view code, const std::string& message) {
    return BridgeError(BridgeError::Bridge{}, BridgeError::isValidCode(code) ? std::string(code) : std::string(bridge_code::generic), message);
}
}  // namespace detail

}  // namespace webfront
