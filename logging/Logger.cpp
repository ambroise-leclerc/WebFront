/** @brief WebFront-owned module-consuming TU; the facade header remains independent of modules. */
import std;
import mddlog.core.loglevel;
import mddlog.adapter.textlogger;
import mddlog.core.record;
import mddlog.core.ring;
import mddlog.adapter.logrecord;
import mddlog.adapter.transportconsumer;

#include "tooling/LoggerApi.hpp"

namespace webfront::log {
struct SinkHandle::Registration {
    mddlog::adapter::TextLogger::Handle handle;
};
struct TransportHandle::Registration {
    mddlog::adapter::TransportConsumer::Handle handle;
    /// Shared with the callback, which clears it when a synchronous throw detaches the transport.
    std::shared_ptr<std::atomic<bool>> attached;
};

namespace {
using mddlog::core::LogLevel;
struct ProducerState {
    Context                                      context;
    std::function<bool(const DiagnosticRecord&)> writer;
    WriteOutcome                                 outcome;
};

/** @brief Access the calling thread's context, writer and most recent outcome. */
ProducerState& producer() {
    static thread_local ProducerState state;
    return state;
}

/** @brief Encode the call direction independently of the component and message. */
std::string_view directionPrefix(CallDirection direction) {
    switch (direction) {
        case CallDirection::CppToJs:
            return "cpp-js:";
        case CallDirection::JsToCpp:
            return "js-cpp:";
        case CallDirection::None:
            return "";
    }
    return "";
}

/** @brief Access the shared synchronous diagnostic text adapter. */
mddlog::adapter::TextLogger& logger() {
    static mddlog::adapter::TextLogger instance;
    return instance;
}

/** @brief Map supported WebFront diagnostic levels explicitly. */
std::optional<LogLevel> diagnosticLevel(LogType level) {
    switch (level) {
        case Error:
            return LogLevel::Error;
        case Warn:
            return LogLevel::Warn;
        case Info:
            return LogLevel::Info;
        case Debug:
            return LogLevel::Debug;
        default:
            return std::nullopt;
    }
}

using TransportRing = mddlog::core::RingLog<transportRingCapacity>;

/** @brief Map a drained record back to WebFront's four display groups. */
std::uint8_t facadeLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Fatal:
        case LogLevel::Error:
            return Error;
        case LogLevel::Warn:
            return Warn;
        case LogLevel::Info:
            return Info;
        case LogLevel::Debug:
        case LogLevel::Trace:
            return Debug;
    }
    return Info;
}

/** @brief Render a drained record exactly as the synchronous facade renders legacy text. */
std::string renderTransportText(const mddlog::core::LogRecord& record) {
    const auto level  = facadeLevel(record.level);
    const char letter = level == Debug ? 'D' : level == Warn ? 'W' : level == Error ? 'E' : 'I';
    if (level == Debug)
        return std::format("[{}] {:%T} | {:16}:{:4} | {}",
                           letter,
                           record.timestamp,
                           std::filesystem::path(record.location.file_name()).filename().string(),
                           record.location.line(),
                           record.message);
    return std::format("[{}] {:%T} | {}", letter, record.timestamp, record.message);
}

class TransportLane;
/** @brief The lane, once created; it is never destroyed so pooled rings outlive every producer. */
constinit std::atomic<TransportLane*> createdLane{nullptr};
/** @brief The lane while its consumer runs; cleared before the consumer stops at exit. */
constinit std::atomic<TransportLane*> activeLane{nullptr};

/** @brief The calling thread's ring; a producer ring returns to the pool when the thread exits. */
struct RingClaim {
    RingClaim()                            = default;
    RingClaim(const RingClaim&)            = delete;
    RingClaim& operator=(const RingClaim&) = delete;
    RingClaim(RingClaim&&)                 = delete;
    RingClaim& operator=(RingClaim&&)      = delete;
    ~RingClaim();

    TransportRing* ring          = nullptr;
    bool           consumerOwned = false;
};

/** @brief Access the calling thread's ring claim. */
RingClaim& ringClaim() {
    static thread_local RingClaim claim;
    return claim;
}

/**
 * @brief Bounded browser transport lane: pooled SPSC producer rings and one consumer thread.
 *
 * Every ring is registered before the consumer thread starts, as TransportConsumer requires.
 * A thread claims one free producer ring on its first emission and returns it on exit; the
 * pool mutex orders each hand-over, so every ring keeps exactly one producer at a time. The
 * consumer thread logs into its own consumer ring, whose dispatch-time records are suppressed.
 * Transports run without any lane lock held.
 */
class TransportLane {
public:
    TransportLane() {
        rings.reserve(transportProducerRings);
        for (std::size_t index = 0; index < transportProducerRings; ++index) {
            rings.push_back(std::make_unique<TransportRing>());
            consumer.addRing(*rings.back());
            available.push_back(rings.back().get());
        }
        consumer.addConsumerRing(consumerRing);
        worker = std::thread([this] {
            run();
        });
    }
    TransportLane(const TransportLane&)            = delete;
    TransportLane& operator=(const TransportLane&) = delete;
    TransportLane(TransportLane&&)                 = delete;
    TransportLane& operator=(TransportLane&&)      = delete;
    ~TransportLane()                               = delete;

    /** @brief Admit one record into the calling thread's ring without waiting for the consumer. */
    WriteStatus write(const mddlog::core::RecordInput& input) {
        auto& claim = ringClaim();
        if (claim.ring == nullptr && (claim.ring = acquire()) == nullptr)
            return WriteStatus::RingUnavailable;
        // capture() validated the identifiers, so a refusal here is saturation.
        if (claim.ring->tryWrite(input).admission() != mddlog::core::Admission::Written)
            return WriteStatus::RingFull;
        if (!pending.exchange(true))
            consumerWake.notify_one();
        return WriteStatus::Written;
    }

    /** @brief Producers skip the rings while no transport could receive their records. */
    [[nodiscard]] bool hasTransports() const noexcept {
        return attachedTransports.load(std::memory_order_acquire) > 0;
    }
    void attach() noexcept {
        attachedTransports.fetch_add(1, std::memory_order_acq_rel);
    }
    /** @brief Count each registration's detachment once; true for the path that detached it. */
    bool detach(std::atomic<bool>& attached) noexcept {
        if (!attached.exchange(false))
            return false;
        attachedTransports.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    /** @brief Return an exiting thread's producer ring to the pool. */
    void release(TransportRing* ring) {
        const std::scoped_lock lock(poolMutex);
        available.push_back(ring);
    }

    /** @brief Wait for a drain that starts after this call; never on the consumer thread. */
    bool flush() {
        if (std::this_thread::get_id() == consumerThread.load())
            return false;
        std::unique_lock lock(cycleMutex);
        if (stopping)
            return false;
        const auto target = started + 1;
        flushTarget       = std::max(flushTarget, target);
        consumerWake.notify_one();
        cycleDone.wait(lock, [&] {
            return stopping || completed >= target;
        });
        return completed >= target;
    }

    /** @brief Stop and join the consumer; pooled rings remain valid for late producers. */
    void stop() {
        {
            const std::scoped_lock lock(cycleMutex);
            stopping = true;
        }
        consumerWake.notify_one();
        cycleDone.notify_all();
        if (worker.joinable())
            worker.join();
    }

    /** @brief Combine TransportConsumer's counters with the pool's own refusals. */
    [[nodiscard]] TransportHealth health() const {
        const auto snapshot = consumer.healthSnapshot();
        return {.delivered        = snapshot.delivered,
                .writeFailures    = snapshot.writeFailures,
                .reportedFailures = snapshot.reportedFailures,
                .detachments      = snapshot.detachments,
                .reentrantRecords = snapshot.reentrantRecords,
                .ringRefusals     = snapshot.ringRefusals,
                .ringUnavailable  = unavailable.load(std::memory_order_relaxed),
                .drainFailures    = drainFailures.load(std::memory_order_relaxed),
                .activeTransports = snapshot.activeTransports};
    }

    mddlog::adapter::TransportConsumer consumer;

private:
    static constexpr auto idleInterval = std::chrono::milliseconds(50);

    TransportRing* acquire() {
        const std::scoped_lock lock(poolMutex);
        if (available.empty()) {
            unavailable.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        auto* ring = available.back();
        available.pop_back();
        return ring;
    }

    void run() {
        consumerThread.store(std::this_thread::get_id());
        auto& claim         = ringClaim();
        claim.ring          = &consumerRing;
        claim.consumerOwned = true;
        std::unique_lock lock(cycleMutex);
        while (!stopping) {
            // A notification can race the predicate check; the timeout bounds that delay.
            consumerWake.wait_for(lock, idleInterval, [this] {
                return stopping || pending.load() || flushTarget > completed;
            });
            if (stopping)
                break;
            pending.store(false);
            const auto cycle = ++started;
            lock.unlock();
            try {
                static_cast<void>(consumer.drainOnce());
            } catch (...) {
                // Unacknowledged records stay in their rings for the next cycle. Logging here
                // would feed the lane that failed; the health counter reports it instead.
                drainFailures.fetch_add(1, std::memory_order_relaxed);
            }
            lock.lock();
            completed = cycle;
            cycleDone.notify_all();
        }
    }

    std::vector<std::unique_ptr<TransportRing>> rings;
    TransportRing                               consumerRing;
    std::mutex                                  poolMutex;
    std::vector<TransportRing*>                 available;
    std::atomic<std::size_t>                    attachedTransports{0};
    std::atomic<std::uint64_t>                  unavailable{0};
    std::atomic<std::uint64_t>                  drainFailures{0};
    std::atomic<bool>                           pending{false};
    std::atomic<std::thread::id>                consumerThread;
    std::mutex                                  cycleMutex;
    std::condition_variable                     consumerWake;
    std::condition_variable                     cycleDone;
    std::uint64_t                               started     = 0;
    std::uint64_t                               completed   = 0;
    std::uint64_t                               flushTarget = 0;
    bool                                        stopping    = false;
    std::thread                                 worker;
};

RingClaim::~RingClaim() {
    if (ring != nullptr && !consumerOwned)
        if (auto* lane = createdLane.load(std::memory_order_acquire))
            lane->release(ring);
}

/** @brief Stops the consumer during static destruction, before the text adapter it may use. */
struct LaneShutdown {
    explicit LaneShutdown(TransportLane& value) : lane(&value) {}
    LaneShutdown(const LaneShutdown&)            = delete;
    LaneShutdown& operator=(const LaneShutdown&) = delete;
    LaneShutdown(LaneShutdown&&)                 = delete;
    LaneShutdown& operator=(LaneShutdown&&)      = delete;
    ~LaneShutdown() {
        activeLane.store(nullptr, std::memory_order_release);
        lane->stop();
    }
    TransportLane* lane;
};

/** @brief Create the lane on first transport registration. */
TransportLane& transportLane() {
    static TransportLane* const instance = [] {
        // Construct the text adapter first so that it outlives the shutdown guard below.
        static_cast<void>(logger());
        // Never deleted: threads still running at exit may hold one of its rings.
        auto* created = new TransportLane;  // NOLINT(cppcoreguidelines-owning-memory)
        createdLane.store(created, std::memory_order_release);
        activeLane.store(created, std::memory_order_release);
        return created;
    }();
    static const LaneShutdown shutdown{*instance};
    return *instance;
}

/** @brief Refuse an overlong identifier before any concatenation allocates; empty when valid. */
std::optional<WriteOutcome> checkIdentifiers(const Context& context, std::string_view prefix) {
    if (context.component.size() > mddlog::core::componentCapacity)
        return WriteOutcome{.status = WriteStatus::IdentifierTooLong, .field = ContextField::Component};
    if (context.callId.size() > mddlog::core::operationIdCapacity - prefix.size())
        return WriteOutcome{.status = WriteStatus::IdentifierTooLong, .field = ContextField::OperationId};
    if (context.webLinkId.size() > mddlog::core::correlationIdCapacity)
        return WriteOutcome{.status = WriteStatus::IdentifierTooLong, .field = ContextField::CorrelationId};
    return std::nullopt;
}

/** @brief Admit the record into this thread's transport ring while a transport is attached. */
WriteStatus submitToLane(const mddlog::core::RecordInput& input) {
    auto* lane = activeLane.load(std::memory_order_acquire);
    if (lane == nullptr || !lane->hasTransports())
        return WriteStatus::Written;
    return lane->write(input);
}

/** @brief Offer the validated snapshot to the host writer; false only when it refuses. */
bool submitToWriter(const ProducerState& state, LogType level, const mddlog::core::RecordInput& input, const mddlog::core::GovernedRecord& captured) {
    if (!state.writer)
        return true;
    return state.writer({.level            = level,
                         .time             = input.time.value(),
                         .location         = input.location,
                         .message          = input.message,
                         .component        = captured.component(),
                         .operationId      = captured.operationId(),
                         .correlationId    = captured.correlationId(),
                         .messageTruncated = captured.truncated().message});
}

/** @brief Validate emission context and synchronously submit its snapshot to the lane and the ring writer. */
WriteOutcome capture(LogType level, std::string_view text, const std::source_location& location) {
    auto&      state  = producer();
    const auto mapped = diagnosticLevel(level);
    if (!mapped || !logger().is(*mapped))
        return state.outcome = {};
    const auto&            context = state.context;
    const std::string_view prefix  = directionPrefix(context.direction);
    if (const auto refused = checkIdentifiers(context, prefix))
        return state.outcome = *refused;
    const std::string               operation = std::string(prefix) + std::string(context.callId);
    const auto                      time      = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    const mddlog::core::RecordInput input{.level         = *mapped,
                                          .time          = mddlog::core::RawTime::available(time),
                                          .location      = location,
                                          .message       = text,
                                          .component     = context.component,
                                          .operationId   = operation,
                                          .correlationId = context.webLinkId};
    mddlog::core::GovernedRecord    captured;
    const bool                      truncated = captured.assign(input).truncated().message;
    WriteOutcome                    outcome{.status = WriteStatus::Written, .messageTruncated = truncated};
    // The lane and the host writer are independent; report the first refusal.
    if (const auto status = submitToLane(input); status != WriteStatus::Written)
        outcome = {.status = status};
    if (!submitToWriter(state, level, input, captured) && outcome.status == WriteStatus::Written)
        outcome = {.status = WriteStatus::RingFull};
    return state.outcome = outcome;
}

constexpr std::size_t hexRowWidth = 16;

/** @brief Append sixteen padded hex bytes with the legacy eight-byte grouping. */
void appendHexColumn(std::string& result, std::span<const std::byte> row) {
    constexpr std::size_t groupWidth = 8;
    for (std::size_t index = 0; index < hexRowWidth; ++index) {
        if (index % groupWidth == 0)
            result += ' ';
        result += index < row.size() ? std::format(" {:02x}", std::to_integer<unsigned int>(row[index])) : "   ";
    }
}

/** @brief Append only available bytes, replacing non-printable ASCII with a dot. */
void appendAsciiColumn(std::string& result, std::span<const std::byte> row) {
    constexpr unsigned int printableStart = 32;
    constexpr unsigned int printableEnd   = 127;
    result                               += ' ';
    for (const auto byte : row) {
        const auto value = std::to_integer<unsigned int>(byte);
        result          += value >= printableStart && value < printableEnd ? static_cast<char>(value) : '.';
    }
}

/** @brief Render the legacy dump with a platform-independent ASCII column. */
std::string hexDump(std::span<const std::byte> bytes) {
    std::string result;
    for (std::size_t address = 0; address < bytes.size(); address += hexRowWidth) {
        const auto row = bytes.subspan(address, std::min(hexRowWidth, bytes.size() - address));
        result        += std::format("{:08x}", address);
        appendHexColumn(result, row);
        appendAsciiColumn(result, row);
        if (address + hexRowWidth < bytes.size())
            result += '\n';
    }
    return result;
}
}  // namespace

/** @brief Install a nested context for the calling thread. */
ContextScope::ContextScope(Context value) noexcept : previous(producer().context) {
    producer().context = value;
}
/** @brief Restore the preceding context when the scope exits. */
ContextScope::~ContextScope() {
    producer().context = previous;
}
/** @brief Bind or clear the calling thread's synchronous ring writer. */
void setRecordWriter(std::function<bool(const DiagnosticRecord&)> writer) {
    producer().writer = std::move(writer);
}
/** @brief Read the calling thread's most recent completed emission outcome. */
WriteOutcome lastWriteOutcome() noexcept {
    return producer().outcome;
}
/** @brief Capture an explicit diagnostic emission and deliver legacy text independently of refusal. */
WriteOutcome tryWrite(LogType level, std::string_view text, const std::source_location& location) {
    const auto result = capture(level, text, location);
    if (result.status != WriteStatus::Filtered) {
        if (const auto mapped = diagnosticLevel(level))
            logger().write(*mapped, text);
    }
    return result;
}

/** @brief Update one diagnostic group or disable all groups. */
void set(LogType level, bool enabled) {
    if (level == Disabled)
        logger().disableAll();
    else if (const auto mapped = diagnosticLevel(level))
        logger().set(*mapped, enabled);
}

/** @brief Query the enabled state of one supported diagnostic group. */
bool is(LogType level) {
    const auto mapped = diagnosticLevel(level);
    return mapped && logger().is(*mapped);
}

/** @brief Apply the WebFront severity threshold to the independent diagnostic groups. */
void setLogLevel(LogType level) {
    // WebFront's order is inverse to mddlog's; no enum cast crosses this boundary.
    set(Error, level >= Error);
    set(Warn, level >= Warn);
    set(Info, level >= Info);
    set(Debug, level >= Debug);
}

/** @brief Register a synchronous text callback with an independent handle. */
SinkHandle addSink(std::function<void(std::string_view)> callback) {
    return SinkHandle(std::make_shared<SinkHandle::Registration>(logger().addSink(std::move(callback))));
}

/** @brief Remove a text callback using the registry's quiescence contract. */
void removeSink(const SinkHandle& handle) {
    if (handle.value)
        logger().removeSink(handle.value->handle);
}

/** @brief Register a transport on the bounded consumer lane. */
TransportHandle addTransport(std::function<void(const TransportRecord&)> write) {
    auto& lane     = transportLane();
    auto  attached = std::make_shared<std::atomic<bool>>(true);
    auto  handle   = lane.consumer.addTransport([&lane, attached, write = std::move(write)](const mddlog::core::LogRecord& record) {
        const auto text = renderTransportText(record);
        try {
            write({.level            = facadeLevel(record.level),
                      .timeAvailable    = record.timeAvailable,
                      .time             = std::chrono::time_point_cast<std::chrono::nanoseconds>(record.timestamp),
                      .location         = record.location,
                      .message          = record.message,
                      .messageTruncated = record.messageTruncated,
                      .component        = record.category,
                      .operationId      = record.operationId,
                      .correlationId    = record.correlationId,
                      .text             = text});
        } catch (...) {
            // TransportConsumer detaches this transport and counts the failure.
            static_cast<void>(lane.detach(*attached));
            throw;
        }
    });
    lane.attach();
    return TransportHandle(std::make_shared<TransportHandle::Registration>(std::move(handle), std::move(attached)));
}

/** @brief Retire a transport through the registry's quiescent removal contract. */
void removeTransport(const TransportHandle& handle) {
    if (auto* lane = createdLane.load(std::memory_order_acquire); lane != nullptr && handle.value) {
        lane->consumer.removeTransport(handle.value->handle);
        static_cast<void>(lane->detach(*handle.value->attached));
    }
}

/** @brief Count a transport failure, then retire it before the caller logs that failure. */
void reportTransportFailure(const TransportHandle& handle) {
    if (auto* lane = createdLane.load(std::memory_order_acquire); lane != nullptr && handle.value) {
        // A report for an already detached transport is not a new failure.
        if (lane->detach(*handle.value->attached))
            lane->consumer.reportFailure(handle.value->handle);
        else
            lane->consumer.removeTransport(handle.value->handle);
    }
}

/** @brief Read the lane's counters without involving any sink. */
TransportHealth transportHealth() {
    auto* lane = createdLane.load(std::memory_order_acquire);
    return lane != nullptr ? lane->health() : TransportHealth{};
}

/** @brief Wait for one complete consumer drain after the call. */
bool flushTransports() {
    auto* lane = activeLane.load(std::memory_order_acquire);
    return lane != nullptr && lane->flush();
}

namespace detail {
/** @brief Route a formatted legacy diagnostic through context capture. */
void write(LogType level, std::string_view text) {
    (void)tryWrite(level, text);
}

/** @brief Capture and render a debug diagnostic with the original caller location. */
void writeDebug(std::string_view text, const std::source_location& location) {
    if (capture(Debug, text, location).status != WriteStatus::Filtered)
        logger().write(LogLevel::Debug, text, location);
}

/** @brief Capture both hex records at the caller's location and preserve independent text delivery. */
HexWriteOutcome writeHex(std::string_view text, std::span<const std::byte> bytes, const std::source_location& location) {
    const auto messageResult = capture(Info, text, location);
    if (messageResult.status == WriteStatus::Filtered)
        return {};
    const auto dump       = hexDump(bytes);
    const auto dumpResult = capture(Info, dump, location);
    auto       combined   = messageResult.status == WriteStatus::Written ? dumpResult : messageResult;
    // Summarize both admissions: a refusal does not erase the other record's truncation.
    combined.messageTruncated = messageResult.messageTruncated || dumpResult.messageTruncated;
    producer().outcome        = combined;
    logger().writeDump(LogLevel::Info, text, dump);
    return {.message = messageResult, .dump = dumpResult};
}
}  // namespace detail
}  // namespace webfront::log
