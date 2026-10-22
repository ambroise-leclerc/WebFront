/// @brief Shared correlation state for future and sender based browser calls.
#pragma once
#include "Messages.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace webfront::detail {

template <http::BuffersPolicyType Policy>
void checkReturnException(const msg::FunctionReturn<Policy>& result, std::span<const std::byte> payload) {
    if (result.getParametersCount() != 1)
        return;
    if (payload.empty())
        return;
    if (static_cast<msg::CodedType>(payload.front()) != msg::CodedType::exception)
        return;
    std::string message;
    result.decodeParameter(message, payload);
    if (!payload.empty())
        throw std::runtime_error("Malformed exception return payload");
    throw std::runtime_error(message);
}

template <http::BuffersPolicyType Policy>
void decodeVoidReturn(const msg::FunctionReturn<Policy>& result, std::span<const std::byte> payload) {
    if (result.getParametersCount() != 0 || !payload.empty())
        throw std::runtime_error("Malformed void return message");
}

template <typename Result, http::BuffersPolicyType Policy>
Result decodeValueReturn(const msg::FunctionReturn<Policy>& result, std::span<const std::byte> payload) {
    if (result.getParametersCount() != 1)
        throw std::runtime_error("Malformed function return message");
    Result value{};
    result.decodeParameter(value, payload);
    if (!payload.empty())
        throw std::runtime_error("Function return contains trailing data");
    return value;
}

template <typename Result, http::BuffersPolicyType Policy>
Result decodeReturn(const msg::FunctionReturn<Policy>& result) {
    auto payload = result.payload();
    checkReturnException(result, payload);
    if constexpr (std::is_void_v<Result>)
        return decodeVoidReturn(result, payload);
    else
        return decodeValueReturn<Result>(result, payload);
}

template <http::BuffersPolicyType Policy>
class PendingCalls {
public:
    struct Call {
        std::function<void(const msg::FunctionReturn<Policy>&)> complete;
        std::function<void(std::exception_ptr)>                 reject;
        const void*                                             owner{};
    };

    msg::CallId add(Call call) {
        std::lock_guard lock(mutex);
        if (closed)
            throw std::runtime_error("Browser connection closed");
        for (std::size_t attempts = 0; attempts < std::numeric_limits<msg::CallId>::max(); ++attempts) {
            const auto candidate = takeNextId();
            if (pending.contains(candidate))
                continue;
            pending.emplace(candidate, std::move(call));
            return candidate;
        }
        throw std::runtime_error("Too many outstanding WebFront calls");
    }

    // Retain only the ID until the browser's late reply arrives. Reusing it earlier
    // could complete a different call after the 16-bit correlation counter wraps.
    void cancel(msg::CallId id, const void* owner = nullptr) {
        std::lock_guard lock(mutex);
        if (auto found = pending.find(id); found != pending.end() && found->second.owner == owner)
            found->second = {};
    }

    bool complete(const msg::FunctionReturn<Policy>& result) {
        Call call;
        {
            std::lock_guard lock(mutex);
            auto            found = pending.find(result.getCallId());
            if (found == pending.end())
                return false;
            call = std::move(found->second);
            pending.erase(found);
        }
        if (call.complete)
            call.complete(result);
        return true;
    }

    void close(std::exception_ptr error) {
        std::map<msg::CallId, Call> calls;
        {
            std::lock_guard lock(mutex);
            closed = true;
            calls.swap(pending);
        }
        for (auto& [id, call] : calls) {
            static_cast<void>(id);
            if (call.reject)
                call.reject(error);
        }
    }

private:
    // The wire reserves zero for calls without a result, including at counter wrap.
    msg::CallId takeNextId() noexcept {
        const auto candidate = nextCallId++;
        if (nextCallId == 0)
            nextCallId = 1;
        return candidate;
    }

    std::mutex                  mutex;
    std::map<msg::CallId, Call> pending;
    msg::CallId                 nextCallId{1};
    bool                        closed{};
};
}  // namespace webfront::detail
