/// @date 31/01/2022 21:19:42
/// @author Ambroise Leclerc
/// @brief WebLink represents a connexion to an HTML/JS renderer
#pragma once
#include "../http/WebSocket.hpp"
#include "../tooling/Logger.hpp"
#include "Messages.hpp"

#include <cstddef>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace webfront {

using WebLinkId = uint16_t;

struct WebLinkEvent {
    enum class Code { linked, closed, cppFunctionCalled };

    WebLinkEvent(Code eventCode, WebLinkId id, std::string message = {}, std::span<const std::byte> dataView = {}, msg::CallId correlationId = 0)
        : code(eventCode), webLinkId(id), text(std::move(message)), data(dataView), callId(correlationId) {}
    Code code;
    WebLinkId webLinkId;
    std::string text;
    std::span<const std::byte> data;
    msg::CallId callId;
};

template<typename Net, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
class WebLink {
    struct PendingCall {
        std::function<void(const msg::FunctionReturn<Policy>&)> complete;
        std::function<void(std::exception_ptr)> reject;
    };

    websocket::WebSocket<Net, Policy> ws;
    WebLinkId id;
    bool sameEndian;
    std::optional<size_t> logSink;
    std::function<void(WebLinkEvent)> eventsHandler;
    std::span<const std::byte> undecodedData; /// Data received but not yet consumed
    std::mutex pendingMutex;
    std::map<msg::CallId, PendingCall> pendingCalls;
    msg::CallId nextCallId{1};
    bool closed{};

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler)
        : ws(std::move(socket)), id(webLinkId), eventsHandler(eventHandler) {
        log::debug("New WebLink created with id:{}", id);

        ws.onMessage([this](std::string_view text) {
            log::debug("onMessage(text) :{}", text);
            ws.write("This is my response");
        });
        ws.onMessage([this](std::span<const std::byte> data) {
            log::infoHex("onMessage(binary) :", data);

            if (data.empty()) {
                log::error("Received an empty WebFront message");
                return;
            }

            switch (static_cast<msg::Command>(data[0])) {
            case msg::Command::handshake: {
                auto command = msg::Handshake::castFromRawData(data);
                sendCommand(msg::Ack{});
                logSink =
                  log::addSinks([this](std::string_view t) { sendCommand(msg::TextCommand(msg::TxtOpcode::debugLog, t)); });
                
                // Use if constexpr to check endianness at compile time
                if constexpr (std::endian::native == std::endian::little) {
                    sameEndian = (static_cast<msg::JSEndian>(command->getEndian()) == msg::JSEndian::little);
                } else if constexpr (std::endian::native == std::endian::big) {
                    sameEndian = (static_cast<msg::JSEndian>(command->getEndian()) == msg::JSEndian::big);
                } else {
                    // Handle mixed-endian if necessary, or assume mismatch
                    sameEndian = false; 
                }

                eventsHandler({WebLinkEvent::Code::linked, id});
            } break;

            case msg::Command::callFunction: {
                log::info("Function called !");
                auto command = msg::FunctionCall<Policy>::castFromRawData(data);
                auto [functionName, paramData] = command->getFunctionName();
                try {
                    eventsHandler(WebLinkEvent(WebLinkEvent::Code::cppFunctionCalled, id, functionName, paramData, command->getCallId()));
                }
                catch (const std::out_of_range& e) {
                    msg::FunctionReturn<Policy> returnValue;
                    returnValue.setCallId(command->getCallId());
                    websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(returnValue.header().data()), returnValue.header().size())};

                    returnValue.encodeParameter(e, frame);
                    if (command->getCallId() != 0) sendFrame(std::move(frame));
                }
                catch (const std::exception& e) {
                    log::info("event cppFunctionCalled failed with exception {}", e.what());
                    sendError(command->getCallId(), e.what());
                }
            } break;

            case msg::Command::functionReturn: {
                auto command = msg::FunctionReturn<Policy>::castFromRawData(data);
                completePending(*command);
            } break;

            default: break;
            }
        });
        ws.onClose([this](websocket::CloseEvent event) {
            auto message = event.reason.empty() ? std::string("Browser connection closed") : std::string("Browser connection closed: ") + event.reason;
            rejectPending(std::make_exception_ptr(std::runtime_error(message)));
        });

        ws.start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;
    WebLink& operator=(const WebLink&) = delete;
    WebLink& operator=(WebLink&&) = delete;

    ~WebLink() {
        log::debug("WebLink destructor");
        rejectPending(std::make_exception_ptr(std::runtime_error("Browser connection closed")));
        if (logSink) log::removeSinks(logSink.value());
    }

    void sendCommand(auto message) { ws.write(message.header(), message.payload()); }
    void sendFrame(websocket::Frame<Net> frame) { ws.write(std::move(frame)); }

    template<typename Result>
    std::pair<msg::CallId, std::future<Result>> expectResult() {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        std::lock_guard lock(pendingMutex);
        if (closed) {
            promise->set_exception(std::make_exception_ptr(std::runtime_error("Browser connection closed")));
            return {0, std::move(future)};
        }

        auto callId = nextAvailableCallId();
        pendingCalls.emplace(callId, PendingCall{
          .complete = [promise](const msg::FunctionReturn<Policy>& result) { settleResult<Result>(*promise, result); },
          .reject   = [promise](std::exception_ptr error) { promise->set_exception(std::move(error)); }});
        return {callId, std::move(future)};
    }

private:
    /// A return message either carries an encoded exception, which is rethrown into the promise, or the
    /// value itself. Anything that fails to decode also surfaces as a broken promise rather than a throw
    /// on the receive thread.
    template<typename Result>
    static void settleResult(std::promise<Result>& promise, const msg::FunctionReturn<Policy>& result) {
        try {
            auto payload = result.payload();
            if (carriesException(result, payload)) rethrowEncodedException(result, payload);
            decodeInto(promise, result, payload);
        }
        catch (...) {
            promise.set_exception(std::current_exception());
        }
    }

    static bool carriesException(const msg::FunctionReturn<Policy>& result, std::span<const std::byte> payload) {
        if (result.getParametersCount() != 1 || payload.empty()) return false;
        return static_cast<msg::CodedType>(payload.front()) == msg::CodedType::exception;
    }

    [[noreturn]] static void rethrowEncodedException(const msg::FunctionReturn<Policy>& result, std::span<const std::byte> payload) {
        std::string message;
        result.decodeParameter(message, payload);
        if (!payload.empty()) throw std::runtime_error("Malformed exception return payload");
        throw std::runtime_error(message);
    }

    template<typename Result>
    static void decodeInto(std::promise<Result>& promise, const msg::FunctionReturn<Policy>& result, std::span<const std::byte>& payload) {
        if constexpr (std::is_void_v<Result>) {
            if (result.getParametersCount() != 0 || !payload.empty()) throw std::runtime_error("Malformed void return message");
            promise.set_value();
        }
        else {
            if (result.getParametersCount() != 1) throw std::runtime_error("Malformed function return message");
            Result value{};
            result.decodeParameter(value, payload);
            if (!payload.empty()) throw std::runtime_error("Function return contains trailing data");
            promise.set_value(std::move(value));
        }
    }

public:
    void sendError(msg::CallId callId, std::string_view message) {
        if (callId == 0) return;
        msg::FunctionReturn<Policy> result;
        result.setCallId(callId);
        websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(result.header().data()), result.header().size())};
        std::runtime_error error{std::string(message)};
        result.encodeParameter(error, frame);
        frame.freeze();
        sendFrame(std::move(frame));
    }

private:
    msg::CallId nextAvailableCallId() {
        for (std::size_t attempts = 0; attempts < std::numeric_limits<msg::CallId>::max(); ++attempts) {
            const auto candidate = nextCallId++;
            if (nextCallId == 0) nextCallId = 1;
            if (!pendingCalls.contains(candidate)) return candidate;
        }
        throw std::runtime_error("Too many outstanding WebFront calls");
    }

    void completePending(const msg::FunctionReturn<Policy>& result) {
        PendingCall pending;
        {
            std::lock_guard lock(pendingMutex);
            auto found = pendingCalls.find(result.getCallId());
            if (found == pendingCalls.end()) {
                log::error("Received a return for unknown call {}", result.getCallId());
                return;
            }
            pending = std::move(found->second);
            pendingCalls.erase(found);
        }
        pending.complete(result);
    }

    void rejectPending(std::exception_ptr error) {
        std::map<msg::CallId, PendingCall> pending;
        {
            std::lock_guard lock(pendingMutex);
            closed = true;
            pending.swap(pendingCalls);
        }
        for (auto& [callId, call] : pending) {
            static_cast<void>(callId);
            call.reject(error);
        }
    }
};

} // namespace webfront
