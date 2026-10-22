/// @date 31/01/2022 21:19:42
/// @author Ambroise Leclerc
/// @brief WebLink represents a connexion to an HTML/JS renderer
#pragma once
#include "../http/WebSocket.hpp"
#include "../tooling/Logger.hpp"
#include "Messages.hpp"
#include "PendingCalls.hpp"

#include <algorithm>
#include <array>
#include <atomic>
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
    std::shared_ptr<HandlerGuard> handlers{std::make_shared<HandlerGuard>()};
    std::shared_ptr<websocket::WebSocket<Net, Policy>> ws;
    WebLinkId id;
    DecimalId idText{id};
    bool sameEndian;
    std::mutex transportMutex;
    std::optional<log::TransportHandle> transport; /// Browser log transport, attached at handshake
    bool transportRetired{};                       /// No transport may attach after disconnection
    std::atomic<std::uint64_t> droppedLogs{0};     /// Diagnostic frames refused by the bounded write queue
    std::function<void(WebLinkEvent)> eventsHandler;
    std::span<const std::byte> undecodedData; /// Data received but not yet consumed
    std::shared_ptr<detail::PendingCalls<Policy>>      pending{std::make_shared<detail::PendingCalls<Policy>>()};

public:
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler, networking::TaskScope& scope)
        : ws(websocket::WebSocket<Net, Policy>::create(std::move(socket), scope)), id(webLinkId), eventsHandler(eventHandler) {
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
                // The WebSocket reports its closure once, and never when it is stopped by the server or the link.
                eventsHandler({WebLinkEvent::Code::closed, id});
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
        // Only once the transport is quiescent (no log write can still start on the socket), close the
        // connection: nothing owns it any more and its pending read must be able to complete.
        ws->stop();
        const log::ContextScope context(linkContext());
        log::debug("WebLink destructor");
        rejectPending(std::make_exception_ptr(std::runtime_error("Browser connection closed")));
    }

    /// Diagnostic frames are refused once this many frames of any kind await the network.
    static constexpr std::size_t maxPendingLogFrames = 64;
    [[nodiscard]] std::uint64_t droppedLogFrames() const { return droppedLogs.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t pendingWrites() const { return ws->pendingWrites(); }

    void sendCommand(auto message) { ws->write(message.header(), message.payload()); }
    void sendFrame(websocket::Frame<Net> frame) { ws->write(std::move(frame)); }

    template<typename Result>
    std::pair<msg::CallId, std::future<Result>> expectResult() {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        msg::CallId callId{};
        try {
            callId = pending->add({.complete =
                                       [promise](const msg::FunctionReturn<Policy>& result) {
                                           try {
                                               if constexpr (std::is_void_v<Result>) {
                                                   detail::decodeReturn<void>(result);
                                                   promise->set_value();
                                               } else
                                                   promise->set_value(detail::decodeReturn<Result>(result));
                                           } catch (...) {
                                               promise->set_exception(std::current_exception());
                                           }
                                       },
                                   .reject =
                                       [promise](std::exception_ptr error) {
                                           promise->set_exception(std::move(error));
                                       }});
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
        return {callId, std::move(future)};
    }

    auto pendingResults() const noexcept {
        return std::weak_ptr(pending);
    }
    auto socket() const noexcept {
        return std::weak_ptr(ws);
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
            // A browser that stops reading must not grow the write queue without bound: diagnostics
            // are dropped and counted, never logged (that would feed this transport again).
            const msg::TextCommand command(msg::TxtOpcode::debugLog, record.text);
            if (!ws->tryWrite(command.header(), command.payload(), maxPendingLogFrames)) {
                droppedLogs.fetch_add(1, std::memory_order_relaxed);
                log::reportTransportOverflow();
            }
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

    void completePending(const msg::FunctionReturn<Policy>& result) {
        const DecimalId callText(result.getCallId());
        const log::ContextScope context({.component = "jsFunction", .webLinkId = idText.view(), .direction = log::CallDirection::CppToJs, .callId = callText.view()});
        if (!pending->complete(result))
            log::error("Received a return for unknown call {}", result.getCallId());
    }

    void rejectPending(std::exception_ptr error) {
        pending->close(std::move(error));
    }
};

} // namespace webfront
