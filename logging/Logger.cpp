/** @brief WebFront-owned module-consuming TU; the facade header remains independent of modules. */
import std;
import mddlog.core.loglevel;
import mddlog.adapter.textlogger;
import mddlog.core.record;

#include "tooling/LoggerApi.hpp"

namespace webfront::log {
struct SinkHandle::Registration {
    mddlog::adapter::TextLogger::Handle handle;
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

/** @brief Validate emission context and synchronously submit its snapshot to the ring writer. */
WriteOutcome capture(LogType level, std::string_view text, const std::source_location& location) {
    auto&      state  = producer();
    const auto mapped = diagnosticLevel(level);
    if (!mapped || !logger().is(*mapped))
        return state.outcome = {};
    const auto&            context = state.context;
    const std::string_view prefix  = directionPrefix(context.direction);
    // Bound the allocation below before concatenating caller-controlled identifiers.
    if (context.component.size() > mddlog::core::componentCapacity)
        return state.outcome = {.status = WriteStatus::IdentifierTooLong, .field = ContextField::Component};
    if (context.callId.size() > mddlog::core::operationIdCapacity - prefix.size())
        return state.outcome = {.status = WriteStatus::IdentifierTooLong, .field = ContextField::OperationId};
    if (context.webLinkId.size() > mddlog::core::correlationIdCapacity)
        return state.outcome = {.status = WriteStatus::IdentifierTooLong, .field = ContextField::CorrelationId};
    const std::string            operation = std::string(prefix) + std::string(context.callId);
    const auto                   time      = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    mddlog::core::GovernedRecord captured;
    const auto                   result    = captured.assign({.level         = *mapped,
                                                              .time          = mddlog::core::RawTime::available(time),
                                                              .location      = location,
                                                              .message       = text,
                                                              .component     = context.component,
                                                              .operationId   = operation,
                                                              .correlationId = context.webLinkId});
    const bool                   truncated = result.truncated().message;
    if (state.writer
        && !state.writer({.level            = level,
                          .time             = time,
                          .location         = location,
                          .message          = text,
                          .component        = captured.component(),
                          .operationId      = captured.operationId(),
                          .correlationId    = captured.correlationId(),
                          .messageTruncated = truncated}))
        return state.outcome = {.status = WriteStatus::RingFull};
    return state.outcome = {.status = WriteStatus::Written, .messageTruncated = truncated};
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
