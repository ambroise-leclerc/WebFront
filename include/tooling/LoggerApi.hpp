/** @brief Facade declarations shared by the ordinary header and the module-consuming TU.
 * Include after the standard headers in Logger.hpp, or after import std.
 */
#pragma once

namespace webfront::log {

using LogType                     = const std::uint8_t;
inline constexpr LogType Disabled = 0, Error = 1, Warn = 2, Info = 3, Debug = 4;
inline const auto        clogSink = [](std::string_view text) {
    std::clog << text << '\n';
};

/** @brief Opaque callback registration; contains no module types in the header. */
class SinkHandle {
public:
    /** @brief Create an empty registration handle. */
    SinkHandle() = default;

private:
    struct Registration;
    /** @brief Wrap one independent callback registration. */
    explicit SinkHandle(std::shared_ptr<Registration> registration) : value(std::move(registration)) {}
    std::shared_ptr<Registration> value;
    friend SinkHandle             addSink(std::function<void(std::string_view)> callback);
    friend void                   removeSink(const SinkHandle& handle);
};

void               set(LogType level, bool enabled);
[[nodiscard]] bool is(LogType level);
/**
 * @brief Update the four independent severity groups in sequence.
 * @note Concurrent is()/write calls may observe a partially applied level change;
 *       there is no atomic update across groups.
 */
void                     setLogLevel(LogType level);
[[nodiscard]] SinkHandle addSink(std::function<void(std::string_view)> callback);
void                     removeSink(const SinkHandle& handle);

/** @brief Explicit connection/call context; views must outlive the scope, never the drain. */
enum class CallDirection : std::uint8_t { None, CppToJs, JsToCpp };
/** @brief Borrowed connection and call fields supplied by the emission scope. */
struct Context {
    std::string_view component;
    std::string_view webLinkId;
    CallDirection    direction = CallDirection::None;
    std::string_view callId;
};

/** @brief Thread-local, nestable context. Restore the previous scope on exit. */
class ContextScope {
public:
    /** @brief Install this thread's context while remembering the enclosing scope. */
    explicit ContextScope(Context value) noexcept;
    /** @brief Restore the enclosing context on normal or exceptional exit. */
    ~ContextScope();
    ContextScope(const ContextScope&)            = delete;
    ContextScope& operator=(const ContextScope&) = delete;
    ContextScope(ContextScope&&)                 = delete;
    ContextScope& operator=(ContextScope&&)      = delete;

private:
    Context previous;
};

/** @brief Admission or filtering result for a diagnostic emission. */
enum class WriteStatus : std::uint8_t { Written, Filtered, IdentifierTooLong, RingFull };
/** @brief Identifier responsible for an IdentifierTooLong refusal. */
enum class ContextField : std::uint8_t { None, Component, OperationId, CorrelationId };
/** @brief Admission status and truncation information for an emission or hex-pair summary. */
struct WriteOutcome {
    WriteStatus  status           = WriteStatus::Filtered;
    ContextField field            = ContextField::None;
    bool         messageTruncated = false;
};

/** @brief Separate admission and truncation results for a hex message and its dump. */
struct HexWriteOutcome {
    WriteOutcome message;
    WriteOutcome dump;
};

/** @brief Validated context and original message. Copy views synchronously into the producer's ring. */
struct DiagnosticRecord {
    std::uint8_t                                    level;
    std::chrono::sys_time<std::chrono::nanoseconds> time;
    std::source_location                            location;
    std::string_view                                message;
    std::string_view                                component;
    std::string_view                                operationId;
    std::string_view                                correlationId;
    bool                                            messageTruncated;
};
/**
 * @brief Bind this thread's sole ring writer; return false only for RingFull.
 * @note Install/remove outside emission. The writer must copy views before returning.
 *       No writer means only legacy text delivery. Setup and the facade may allocate.
 *       Writer exceptions propagate without updating lastWriteOutcome() or delivering text.
 *       The host must remove its binding before ring destruction, including exceptional exits.
 */
void setRecordWriter(std::function<bool(const DiagnosticRecord&)> writer);
/**
 * @brief Observe the last emission on this thread, including refusals from void legacy calls.
 * @note For infoHex(), status/field describe the first refusal, or the dump if both records
 *       are written. messageTruncated is the OR of truncation flags from admitted records,
 *       even when the summary status is a refusal. Use HexWriteOutcome for per-record flags.
 */
[[nodiscard]] WriteOutcome lastWriteOutcome() noexcept;
/** @brief Emit already formatted diagnostic text, capturing context and an explicit location. */
[[nodiscard]] WriteOutcome tryWrite(LogType level, std::string_view text, const std::source_location& location = std::source_location::current());

namespace detail {
void            write(LogType level, std::string_view text);
void            writeDebug(std::string_view text, const std::source_location& location);
HexWriteOutcome writeHex(std::string_view text, std::span<const std::byte> bytes, const std::source_location& location);
}  // namespace detail

template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
struct debug {  // NOLINT(readability-identifier-naming): preserve WebFront's CTAD call-site API.
    debug(std::string_view fmt, Ts&&... args, const std::source_location& location = std::source_location::current()) {
        if (is(Debug))
            detail::writeDebug(std::vformat(fmt, std::make_format_args(args...)), location);
    }
};
template <typename... Ts>
debug(std::string_view, Ts&&...) -> debug<Ts...>;

template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
void info(std::string_view fmt, Ts&&... args) {
    if (is(Info))
        detail::write(Info, std::vformat(fmt, std::make_format_args(args...)));
}
template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
void warn(std::string_view fmt, Ts&&... args) {
    if (is(Warn))
        detail::write(Warn, std::vformat(fmt, std::make_format_args(args...)));
}
template <typename... Ts>
    requires(std::formattable<Ts, char> && ...)
void error(std::string_view fmt, Ts&&... args) {
    if (is(Error))
        detail::write(Error, std::vformat(fmt, std::make_format_args(args...)));
}

/**
 * @brief Emit the complete legacy hex dump and return each structured record's outcome.
 * @note Both records capture the caller's location. The structured dump can be truncated;
 *       inspect dump.messageTruncated. Ignoring the result preserves existing call syntax.
 */
template <std::ranges::contiguous_range Container>
    requires std::ranges::sized_range<Container>
             && (std::is_arithmetic_v<std::ranges::range_value_t<Container>> || std::same_as<std::ranges::range_value_t<Container>, std::byte>)
HexWriteOutcome infoHex(std::string_view text, const Container& container, const std::source_location& location = std::source_location::current()) {
    if (!is(Info))
        return {};
    return detail::writeHex(text, std::as_bytes(std::span(std::ranges::data(container), std::ranges::size(container))), location);
}

template <typename... Callbacks>
    requires(sizeof...(Callbacks) > 0) && (std::constructible_from<std::function<void(std::string_view)>, Callbacks> && ...)
auto addSinks(Callbacks&&... callbacks) {
    if constexpr (sizeof...(Callbacks) == 1)
        return (addSink(std::forward<Callbacks>(callbacks)), ...);
    else
        return std::array<SinkHandle, sizeof...(Callbacks)>{addSink(std::forward<Callbacks>(callbacks))...};
}

template <typename... Handles>
    requires(std::same_as<std::remove_cvref_t<Handles>, SinkHandle> && ...)
void removeSinks(Handles&&... handles) {
    (removeSink(handles), ...);
}

}  // namespace webfront::log
