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
#include <functional>
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
/// Serial number of a connection, never reused (0 is assigned to none): tells apart the connections that share an identifier over time.
using WebLinkGeneration = uint64_t;

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

/// The only size of handshake proof that is kept: the 64 hexadecimal characters of a launch secret (ADR-020).
inline constexpr std::size_t handshakeProofSize = 64;

template<typename Net, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
class WebLink {
    std::shared_ptr<HandlerGuard> handlers{std::make_shared<HandlerGuard>()};
    std::shared_ptr<websocket::WebSocket<Net, Policy>> ws;
    WebLinkId id;
    DecimalId idText{id};
    bool sameEndian;
    bool handshaken{};  /// The handshake is accepted once
    std::mutex transportMutex;
    std::optional<log::TransportHandle> transport; /// Browser log transport, attached at handshake
    bool transportRetired{};                       /// No transport may attach after disconnection
    std::atomic<std::uint64_t> droppedLogs{0};     /// Diagnostic frames refused by the bounded write queue
    std::function<void(WebLinkEvent)> eventsHandler;
    std::span<const std::byte> undecodedData; /// Data received but not yet consumed
    std::shared_ptr<detail::PendingCalls<Policy>>      pending{std::make_shared<detail::PendingCalls<Policy>>()};

public:
    /// Starts reading at once, unless `startNow` is false: start() then begins, once the owner can dispatch its events.
    WebLink(typename Net::Socket&& socket, WebLinkId webLinkId, std::function<void(WebLinkEvent)> eventHandler, networking::TaskScope& scope,
            bool startNow = true)
        : ws(websocket::WebSocket<Net, Policy>::create(std::move(socket), scope)), id(webLinkId), eventsHandler(eventHandler) {
        log::debug("New WebLink created with id:{}", id);

        ws->onMessage([](std::string_view text) { log::debug("onMessage(text) :{}", text); });
        // Handlers capture the shared guard, never rely on `this` alone: completions may run after destruction.
        ws->onMessage([this, guard = handlers](std::span<const std::byte> data) { guard->run([&] { onBinaryMessage(data); }); });
        // Retire the transport before the WebSocket logs the write error, so it is never fed its own failure.
        ws->onWriteError([this, guard = handlers](std::error_code) { guard->run([&] { detachTransport(true); }); });
        ws->onWritten([this, guard = handlers] { guard->run([&] { wakeWriters(); }); });
        ws->onClose([this, guard = handlers](websocket::CloseEvent event) {
            guard->run([&] {
                const log::ContextScope context(linkContext());
                detachTransport(false);
                auto message = event.reason.empty() ? std::string("Browser connection closed") : std::string("Browser connection closed: ") + event.reason;
                rejectPending(std::make_exception_ptr(detail::disconnectedError(message)));
                // The WebSocket reports its closure once, and never when it is stopped by the server or the link.
                const auto queue = writerQueue;  // the event may release this link
                eventsHandler({WebLinkEvent::Code::closed, id});
                wakeAll(*queue);  // after the closure is known: nothing will be written any more
            });
        });

        if (startNow)
            ws->start();
    }

    /// Starts reading, for a link constructed with `startNow` false.
    void start() { ws->start(); }
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
        rejectPending(std::make_exception_ptr(detail::disconnectedError()));
        wakeAllWriters();
    }

    /// Diagnostic frames are refused once this many frames of any kind await the network.
    static constexpr std::size_t maxPendingLogFrames = 64;
    [[nodiscard]] std::uint64_t droppedLogFrames() const { return droppedLogs.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t pendingWrites() const { return ws->pendingWrites(); }

    /// Returns 0 when at most `limit` frames await the network. Otherwise registers `ready`, called once they do, on
    /// the thread that writes the frame bringing them down, and returns its identifier for cancelWrites(). The waiters
    /// are also called when the connection closes or the link is destroyed: nothing is left to wait for.
    std::uint64_t whenWritesAtMost(std::size_t limit, std::function<void()> ready) {
        std::lock_guard lock(writerQueue->mutex);
        if (ws->pendingWrites() <= limit)
            return 0;
        const auto waiter = ++writerQueue->last;
        writerQueue->writers.emplace(waiter, Writer{limit, std::move(ready)});
        return waiter;
    }

    /// Forgets a waiter of whenWritesAtMost() that was not called yet.
    void cancelWrites(std::uint64_t waiter) {
        std::function<void()> removed;  // destroyed outside the lock
        std::lock_guard lock(writerQueue->mutex);
        if (auto found = writerQueue->writers.find(waiter); found != writerQueue->writers.end()) {
            removed = std::move(found->second.ready);
            writerQueue->writers.erase(found);
        }
    }

    void sendCommand(const auto& message) { ws->write(message.header(), message.payload()); }
    void sendFrame(websocket::Frame<Net> frame) { ws->write(std::move(frame)); }
    /// A frame shared with other links (websocket::Frame::share()), never copied.
    void sendFrame(std::shared_ptr<const websocket::Frame<Net>> frame) {
        ws->write(std::move(frame));
    }
    /// Keeps at most `maxPending` frames of `key` queued and only the latest of the others: see WebSocket::writeLatest.
    void sendLatest(std::shared_ptr<const websocket::Frame<Net>> frame, std::string_view key, std::size_t maxPending) {
        ws->writeLatest(std::move(frame), key, maxPending);
    }

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
        // Log records reach connected browsers, and a message can carry a credential (the launch secret of a handshake, a
        // read URL returned by a callback): only the command byte and the size are logged, never the payload.
        log::info("onMessage(binary) : {} bytes, command {}", data.size(), data.empty() ? -1 : static_cast<int>(data[0]));
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
        // One handshake per connection: a repeated one must not reset the stage or the association the first established.
        if (std::exchange(handshaken, true)) {
            log::error("Ignoring a repeated handshake");
            return;
        }
        auto command = msg::Handshake::castFromRawData(data);
        sendCommand(msg::Ack{});
        attachTransport();

        // On a mixed-endian target native matches neither, leaving sameEndian false as it should.
        constexpr bool nativeIsLittle = std::endian::native == std::endian::little;
        constexpr bool nativeIsBig    = std::endian::native == std::endian::big;
        const auto     jsEndian       = static_cast<msg::JSEndian>(command->getEndian());
        sameEndian = (nativeIsLittle && jsEndian == msg::JSEndian::little) || (nativeIsBig && jsEndian == msg::JSEndian::big);

        // Anything after the two header bytes is the secret of an embedded view (ADR-020); the application judges it.
        // Only a proof of the exact size of a launch secret is copied: anything else cannot match and is not worth an allocation.
        auto proof = data.size() > sizeof(msg::Handshake) ? data.subspan(sizeof(msg::Handshake)) : std::span<const std::byte>{};
        if (proof.size() != handshakeProofSize)
            proof = {};
        eventsHandler({WebLinkEvent::Code::linked, id, std::string(reinterpret_cast<const char*>(proof.data()), proof.size())});
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
    void sendError(msg::CallId callId, std::string_view message, std::string_view code = bridge_code::generic) {
        if (callId == 0) return;
        msg::FunctionReturn<Policy> result;
        result.setCallId(callId);
        websocket::Frame<Net> frame{std::span(reinterpret_cast<const std::byte*>(result.header().data()), result.header().size())};
        const auto error = detail::makeBridgeError(code, std::string(message));
        result.encodeParameter(error, frame);
        frame.freeze();
        sendFrame(std::move(frame));
    }

private:
    struct Writer {
        std::size_t           limit;
        std::function<void()> ready;
    };
    /// The waiters of whenWritesAtMost(), by identifier. Shared, so that the closure handler can wake them after its event
    /// released the link (an application whose scheduler runs the release at once destroys the link inside that handler).
    struct WriterQueue {
        std::mutex                      mutex;
        std::map<std::uint64_t, Writer> writers;
        std::uint64_t                   last = 0;
    };
    std::shared_ptr<WriterQueue> writerQueue = std::make_shared<WriterQueue>();

    // The queue shrank: the waiters it now satisfies are removed, then called outside the lock.
    void wakeWriters() {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard lock(writerQueue->mutex);
            if (writerQueue->writers.empty())
                return;
            const auto queued = ws->pendingWrites();
            for (auto writer = writerQueue->writers.begin(); writer != writerQueue->writers.end();) {
                if (queued > writer->second.limit) {
                    ++writer;
                    continue;
                }
                ready.push_back(std::move(writer->second.ready));
                writer = writerQueue->writers.erase(writer);
            }
        }
        for (auto& call : ready)
            call();
    }

    void wakeAllWriters() { wakeAll(*writerQueue); }

    static void wakeAll(WriterQueue& queue) {
        std::map<std::uint64_t, Writer> ready;
        {
            std::lock_guard lock(queue.mutex);
            ready.swap(queue.writers);
        }
        for (auto& [waiter, writer] : ready)
            writer.ready();
    }

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


namespace detail {
/// Completes once at most `limit` frames await the network on a link (WebLink::whenWritesAtMost), or stopped by the
/// receiver's stop token. A link that is gone completes it at once: nothing is left to wait for.
template <typename Link>
class WritesAtMost {
public:
    using sender_concept        = networking::ex::sender_t;
    using completion_signatures = networking::ex::completion_signatures<networking::ex::set_value_t(), networking::ex::set_stopped_t()>;

    WritesAtMost(std::weak_ptr<Link> waited, std::size_t pending) : link(std::move(waited)), limit(pending) {}

    template <typename Receiver>
    class Operation {
        using StopToken = networking::ex::stop_token_of_t<networking::ex::env_of_t<Receiver>>;
        enum class Completion { none, value, stopped };
        // Shared with the link's waiter and the stop callback, which may run on other threads, also during start().
        // The first of them to finish the operation completes it, unless start() is still running: it then records
        // the completion, which start() delivers once it no longer touches the operation.
        struct State {
            std::atomic<bool>          done{false};
            std::atomic<std::uint64_t> waiter{0};
            std::mutex                 mutex;
            bool                       starting   = true;
            Completion                 completion = Completion::none;
            Operation*                 operation;

            // True when start() will deliver the completion itself.
            bool record(Completion finished) {
                std::lock_guard lock(mutex);
                if (starting)
                    completion = finished;
                return starting;
            }
        };
        struct OnStop {
            Operation* operation;
            void       operator()() noexcept { operation->stopped(); }
        };

        Receiver                                                        receiver;
        std::weak_ptr<Link>                                             link;
        std::size_t                                                     limit;
        std::shared_ptr<State>                                          state;
        std::optional<networking::ex::stop_callback_for_t<StopToken, OnStop>> stopCallback;

        void stopped() noexcept {
            if (state->done.exchange(true))
                return;
            if (auto waited = link.lock())
                waited->cancelWrites(state->waiter);
            if (state->record(Completion::stopped))
                return;
            networking::ex::set_stopped(std::move(receiver));  // may destroy this operation
        }

    public:
        Operation(Receiver r, std::weak_ptr<Link> waited, std::size_t pending)
            : receiver(std::move(r)), link(std::move(waited)), limit(pending), state(std::make_shared<State>()) {
            state->operation = this;
        }
        Operation(Operation&&) = delete;

        void start() noexcept {
            auto token = networking::ex::get_stop_token(networking::ex::get_env(receiver));
            auto waited = link.lock();
            if (token.stop_requested()) {
                state->done = true;
                return networking::ex::set_stopped(std::move(receiver));
            }
            if (!waited) {
                state->done = true;
                return networking::ex::set_value(std::move(receiver));
            }
            const auto shared = state;
            stopCallback.emplace(std::move(token), OnStop{this});  // a stop requested meanwhile is recorded at once
            std::uint64_t waiter = 0;
            if (!shared->done) {
                try {
                    waiter = waited->whenWritesAtMost(limit, [shared] {
                        if (shared->done.exchange(true) || shared->record(Completion::value))
                            return;  // stopped first, or start() delivers it
                        auto* operation = shared->operation;
                        operation->stopCallback.reset();  // waits for a stop callback running on another thread
                        networking::ex::set_value(std::move(operation->receiver));
                    });
                } catch (...) { }  // registering failed: there is nothing to wait for
            }
            shared->waiter = waiter;
            Completion completion;
            {
                std::lock_guard lock(shared->mutex);
                shared->starting = false;
                completion       = shared->completion;
            }
            if (completion == Completion::none && waiter == 0 && !shared->done.exchange(true))
                completion = Completion::value;  // already at most `limit` frames queued
            if (completion == Completion::none)
                return;  // the waiter or the stop callback completes it
            if (waiter != 0)
                waited->cancelWrites(waiter);
            stopCallback.reset();
            if (completion == Completion::value)
                networking::ex::set_value(std::move(receiver));
            else
                networking::ex::set_stopped(std::move(receiver));
        }
    };

    template <typename Receiver>
    Operation<Receiver> connect(Receiver receiver) const {
        return Operation<Receiver>(std::move(receiver), link, limit);
    }

private:
    std::weak_ptr<Link> link;
    std::size_t         limit;
};
}  // namespace detail

} // namespace webfront
