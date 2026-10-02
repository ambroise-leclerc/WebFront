/// @date 31/01/2022 21:19:42
/// @author Ambroise Leclerc
/// @brief WebLink represents a connexion to an HTML/JS renderer
#pragma once
#include "../http/WebSocket.hpp"
#include "../tooling/Logger.hpp"
#include "Messages.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

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

/// Decimal text of a link or call identifier for the diagnostic context, without allocating.
class DecimalId {
public:
    explicit DecimalId(std::uint64_t value) : size(static_cast<std::size_t>(std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr - digits.data())) {}
    [[nodiscard]] std::string_view view() const { return {digits.data(), size}; }

private:
    std::array<char, 20> digits{};
    std::size_t size;
};

/// Shared by a WebLink's WebSocket handlers, which can run after the link is destroyed: the
/// WebSocket outlives it while asynchronous operations complete. A handler starting after
/// retire() returns without touching the link; retire() waits for handlers already running.
class HandlerGuard {
public:
    template<typename Handler>
    void run(Handler&& handler) {
        {
            std::lock_guard lock(mutex);
            if (!alive) return;
            ++inFlight;
        }
        struct Exit {
            explicit Exit(HandlerGuard& value) : guard(value) {}
            HandlerGuard& guard;
            Exit(const Exit&) = delete;
            Exit& operator=(const Exit&) = delete;
            ~Exit() {
                auto& running = runningOnThisThread();
                running.erase(std::find(running.begin(), running.end(), &guard));
                std::lock_guard lock(guard.mutex);
                --guard.inFlight;
                guard.idle.notify_all();
            }
        };
        runningOnThisThread().push_back(this);
        const Exit exit{*this};
        std::forward<Handler>(handler)();
    }

    /// Stop new handlers and wait for those on other threads. A handler of this link running on
    /// the calling thread (destruction from inside it) cannot be waited for and is not.
    void retire() {
        const auto& running = runningOnThisThread();
        const auto  own     = static_cast<std::size_t>(std::count(running.begin(), running.end(), this));
        std::unique_lock lock(mutex);
        alive = false;
        idle.wait(lock, [&] { return inFlight == own; });
    }

private:
    static std::vector<const HandlerGuard*>& runningOnThisThread() {
        static thread_local std::vector<const HandlerGuard*> running;
        return running;
    }

    std::mutex mutex;
    std::condition_variable idle;
    bool alive = true;
    std::size_t inFlight = 0;
};

template<typename Net, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
class WebLink {
    struct PendingCall {
        std::function<void(const msg::FunctionReturn<Policy>&)> complete;
        std::function<void(std::exception_ptr)> reject;
    };

    std::shared_ptr<HandlerGuard> handlers{std::make_shared<HandlerGuard>()};
    std::shared_ptr<websocket::WebSocket<Net, Policy>> ws;
    WebLinkId id;
    DecimalId idText{id};
    bool sameEndian;
    std::mutex transportMutex;
    std::optional<log::TransportHandle> transport; /// Browser log transport, attached at handshake
    bool transportRetired{};                       /// No transport may attach after disconnection
    std::function<void(WebLinkEvent)> eventsHandler;
    std::span<const std::byte> undecodedData; /// Data received but not yet consumed
    std::mutex pendingMutex;
    std::map<msg::CallId, PendingCall> pendingCalls;
    msg::CallId nextCallId{1};
    bool closed{};

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler)
        : ws(websocket::WebSocket<Net, Policy>::create(std::move(socket))), id(webLinkId), eventsHandler(eventHandler) {
        log::debug("New WebLink created with id:{}", id);

        ws->onMessage([](std::string_view text) { log::debug("onMessage(text) :{}", text); });
        // Handlers capture the shared guard, never rely on `this` alone: completions may run after destruction.
        ws->onMessage([this, guard = handlers](std::span<const std::byte> data) { guard->run([&] { onBinaryMessage(data); }); });
        // Retire the transport before the WebSocket logs the write error, so it is never fed its own failure.
        ws->onWriteError([this, guard = handlers](std::error_code) { guard->run([&] { detachTransport(true); }); });
        ws->onClose([this, guard = handlers](websocket::CloseEvent event) {
            guard->run([&] {
                const log::ContextScope context(linkContext());
                detachTransport(false);
                auto message = event.reason.empty() ? std::string("Browser connection closed") : std::string("Browser connection closed: ") + event.reason;
                rejectPending(std::make_exception_ptr(std::runtime_error(message)));
            });
        });

        ws->start();
    }
    WebLink(const WebLink&) = delete;
    WebLink(WebLink&&) = delete;
    WebLink& operator=(const WebLink&) = delete;
    WebLink& operator=(WebLink&&) = delete;

    ~WebLink() {
        // First let no WebSocket handler reach this object again, then remove the transport (waiting
        // for its in-flight write) before logging or releasing what it captures. No closed event
        // precedes destruction.
        handlers->retire();
        detachTransport(false);
        const log::ContextScope context(linkContext());
        log::debug("WebLink destructor");
        rejectPending(std::make_exception_ptr(std::runtime_error("Browser connection closed")));
    }

    void sendCommand(auto message) { ws->write(message.header(), message.payload()); }
    void sendFrame(websocket::Frame<Net> frame) { ws->write(std::move(frame)); }

    template<typename Result>
    std::pair<msg::CallId, std::future<Result>> expectResult() {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        std::lock_guard lock(pendingMutex);
        if (closed) {
            promise->set_exception(std::make_exception_ptr(std::runtime_error("Browser connection closed")));
            // Explicitly typed: a bare 0 is an int and narrows into CallId, which MSVC rejects (C4242).
            return {msg::CallId{0}, std::move(future)};
        }

        auto callId = nextAvailableCallId();
        pendingCalls.emplace(callId, PendingCall{
          .complete = [promise](const msg::FunctionReturn<Policy>& result) { settleResult<Result>(*promise, result); },
          .reject   = [promise](std::exception_ptr error) { promise->set_exception(std::move(error)); }});
        return {callId, std::move(future)};
    }

private:
    void onBinaryMessage(std::span<const std::byte> data) {
        const log::ContextScope context(linkContext());
        log::infoHex("onMessage(binary) :", data);
        if (data.empty()) {
            log::error("Received an empty WebFront message");
            return;
        }

        switch (static_cast<msg::Command>(data[0])) {
        case msg::Command::handshake: handleHandshake(data); break;
        case msg::Command::callFunction: handleCallFunction(data); break;
        case msg::Command::functionReturn: completePending(*msg::FunctionReturn<Policy>::castFromRawData(data)); break;
        default: break;
        }
    }

    void handleHandshake(std::span<const std::byte> data) {
        auto command = msg::Handshake::castFromRawData(data);
        sendCommand(msg::Ack{});
        attachTransport();

        // On a mixed-endian target native matches neither, leaving sameEndian false as it should.
        constexpr bool nativeIsLittle = std::endian::native == std::endian::little;
        constexpr bool nativeIsBig    = std::endian::native == std::endian::big;
        const auto     jsEndian       = static_cast<msg::JSEndian>(command->getEndian());
        sameEndian = (nativeIsLittle && jsEndian == msg::JSEndian::little) || (nativeIsBig && jsEndian == msg::JSEndian::big);

        eventsHandler({WebLinkEvent::Code::linked, id});
    }

    void handleCallFunction(std::span<const std::byte> data) {
        auto command = msg::FunctionCall<Policy>::castFromRawData(data);
        const DecimalId callText(command->getCallId());
        const log::ContextScope context({.component = "cppFunction", .webLinkId = idText.view(), .direction = log::CallDirection::JsToCpp, .callId = callText.view()});
        log::info("Function called !");
        auto [functionName, paramData] = command->getFunctionName();
        try {
            eventsHandler(WebLinkEvent(WebLinkEvent::Code::cppFunctionCalled, id, functionName, paramData, command->getCallId()));
        }
        catch (const std::out_of_range& e) {
            sendException(command->getCallId(), e);
        }
        catch (const std::exception& e) {
            log::info("event cppFunctionCalled failed with exception {}", e.what());
            sendError(command->getCallId(), e.what());
        }
    }

    /// An unknown function name is reported back as an encoded exception rather than a plain error string.
    void sendException(msg::CallId callId, const std::out_of_range& error) {
        msg::FunctionReturn<Policy> returnValue;
        returnValue.setCallId(callId);
        websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(returnValue.header().data()), returnValue.header().size())};
        returnValue.encodeParameter(error, frame);
        if (callId != 0) sendFrame(std::move(frame));
    }

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
    /// Context for link-level logs outside any call; operationId stays empty.
    [[nodiscard]] log::Context linkContext() const {
        return {.component = "weblink", .webLinkId = idText.view(), .direction = log::CallDirection::None, .callId = {}};
    }

    void attachTransport() {
        auto handle = log::addTransport([this](const log::TransportRecord& record) {
            sendCommand(msg::TextCommand(msg::TxtOpcode::debugLog, record.text));
        });
        std::optional<log::TransportHandle> replaced{handle};
        {
            std::lock_guard lock(transportMutex);
            if (!transportRetired) replaced = std::exchange(transport, std::move(handle));
        }
        if (replaced) log::removeTransport(*replaced);
    }

    /// Detach at most once; no transport attaches afterwards. A failure is reported, retiring
    /// the transport, before the caller logs anything about it.
    void detachTransport(bool failed) {
        std::optional<log::TransportHandle> attached;
        {
            std::lock_guard lock(transportMutex);
            transportRetired = true;
            attached.swap(transport);
        }
        if (!attached) return;
        if (failed) log::reportTransportFailure(*attached);
        else log::removeTransport(*attached);
    }

    msg::CallId nextAvailableCallId() {
        for (std::size_t attempts = 0; attempts < std::numeric_limits<msg::CallId>::max(); ++attempts) {
            const auto candidate = nextCallId++;
            if (nextCallId == 0) nextCallId = 1;
            if (!pendingCalls.contains(candidate)) return candidate;
        }
        throw std::runtime_error("Too many outstanding WebFront calls");
    }

    void completePending(const msg::FunctionReturn<Policy>& result) {
        const DecimalId callText(result.getCallId());
        const log::ContextScope context({.component = "jsFunction", .webLinkId = idText.view(), .direction = log::CallDirection::CppToJs, .callId = callText.view()});
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
