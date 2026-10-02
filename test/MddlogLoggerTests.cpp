/** @brief Module-free consumer regressions for WebFront's optional diagnostic facade. */
#include "tooling/Logger.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct LoggingScope {
    LoggingScope() {
        webfront::log::setLogLevel(webfront::log::Disabled);
    }
    LoggingScope(const LoggingScope&)            = delete;
    LoggingScope& operator=(const LoggingScope&) = delete;
    LoggingScope(LoggingScope&&)                 = delete;
    LoggingScope& operator=(LoggingScope&&)      = delete;
    ~LoggingScope() {
        webfront::log::setRecordWriter({});
        webfront::log::setLogLevel(webfront::log::Disabled);
        for (const auto& handle : handles)
            webfront::log::removeSinks(handle);
    }
    std::vector<webfront::log::SinkHandle> handles;
};
}  // namespace

SCENARIO("mddlog facade masks, caller location and independent registrations", "[mddlog][logger]") {
    namespace log = webfront::log;
    std::vector<std::string> lines;
    std::size_t              otherWrites = 0;
    LoggingScope             scope;
    for (const auto level : {log::Error, log::Warn, log::Info, log::Debug})
        REQUIRE_FALSE(log::is(level));
    const auto handles = log::addSinks(
        [&](std::string_view text) {
            lines.emplace_back(text);
        },
        [&](std::string_view) {
            ++otherWrites;
        });
    scope.handles.assign(handles.begin(), handles.end());
    log::set(log::Error, true);
    log::set(log::Warn, false);
    log::error("error {}", 42);
    log::warn("invalid {");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].starts_with("[E]"));
    REQUIRE(lines[0].ends_with(" | error 42"));
    log::setLogLevel(log::Debug);
    const auto callerLine = std::source_location::current().line() + 1;
    log::debug("answer {}", 42);
    REQUIRE(lines.back().find(std::format("{:16}:{:4} |", "MddlogLoggerTests.cpp", callerLine)) != std::string::npos);
    log::info("info {}", 42);
    REQUIRE(lines.back().starts_with("[I]"));
    REQUIRE(!lines.back().contains("MddlogLoggerTests.cpp"));
    log::removeSinks(handles[0]);
    log::warn("remaining");
    REQUIRE(lines.size() == 3);
    REQUIRE(otherWrites == 4);
    log::removeSinks(handles[0]);
    log::set(log::Disabled, true);
    REQUIRE_FALSE(log::is(log::Error));
    REQUIRE_FALSE(log::is(log::Debug));
    log::info("invalid {");
    REQUIRE(otherWrites == 4);
}

SCENARIO("mddlog facade renders every byte independently of char signedness", "[mddlog][logger]") {
    namespace log = webfront::log;
    std::vector<std::string> lines;
    LoggingScope             scope;
    scope.handles.push_back(log::addSinks([&](std::string_view text) {
        lines.emplace_back(text);
    }));
    log::setLogLevel(log::Info);
    std::array<std::byte, 256> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>(index);
    log::infoHex("all bytes {literal}", bytes);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].ends_with(" | all bytes {literal}"));
    REQUIRE(lines[1].size() == (16 * 75) + 15);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto expected = index >= 0x20 && index <= 0x7E ? static_cast<char>(index) : '.';
        REQUIRE(lines[1][((index / 16) * 76) + 59 + (index % 16)] == expected);
    }
    log::set(log::Info, false);
    log::infoHex("disabled", bytes);
    REQUIRE(lines.size() == 2);
}

SCENARIO("mddlog facade preserves structured context and independent text delivery", "[mddlog][logger]") {
    namespace log = webfront::log;
    std::vector<std::string> lines;
    std::vector<std::string> operations;
    LoggingScope             scope;
    scope.handles.push_back(log::addSinks([&](std::string_view text) {
        lines.emplace_back(text);
    }));
    log::setLogLevel(log::Info);
    bool admit = true;
    log::setRecordWriter([&](const log::DiagnosticRecord& record) {
        REQUIRE(record.component == "jsFunction");
        REQUIRE(record.correlationId == "2");
        operations.emplace_back(record.operationId);
        return admit;
    });
    {
        const log::ContextScope context({.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::CppToJs, .callId = "1"});
        REQUIRE(log::tryWrite(log::Info, "request").status == log::WriteStatus::Written);
        {
            const log::ContextScope nested({.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::JsToCpp, .callId = "3"});
            log::info("reply");
        }
        admit             = false;
        const auto result = log::infoHex("bytes", std::array<unsigned char, 2>{0, 65});
        REQUIRE(result.message.status == log::WriteStatus::RingFull);
        REQUIRE(result.dump.status == log::WriteStatus::RingFull);
        REQUIRE(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
    }
    REQUIRE(operations == std::vector<std::string>{"cpp-js:1", "js-cpp:3", "cpp-js:1", "cpp-js:1"});
    REQUIRE(lines.size() == 4);
    REQUIRE(lines.back() == "00000000  00 41                                            .A");
    const std::string oversized(33, 'x');
    {
        const log::ContextScope context({.component = oversized, .webLinkId = "2", .direction = log::CallDirection::None, .callId = ""});
        const auto              result = log::tryWrite(log::Info, "refused context");
        REQUIRE(result.status == log::WriteStatus::IdentifierTooLong);
        REQUIRE(result.field == log::ContextField::Component);
    }
    REQUIRE(operations.size() == 4);
    REQUIRE(lines.size() == 5);
}

SCENARIO("mddlog hex summary retains truncation from admitted records only", "[mddlog][logger]") {
    namespace log = webfront::log;
    const LoggingScope scope;
    log::setLogLevel(log::Info);
    const std::array<std::byte, 48> largeDump{};
    const std::array<std::byte, 1>  smallDump{};
    const std::string               largeMessage(161, 'm');
    unsigned int                    writes = 0;
    log::HexWriteOutcome            result;

    SECTION("a refused message remains unmarked while an admitted dump is truncated") {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            return ++writes != 1;
        });
        result = log::infoHex("refused message", largeDump);
        REQUIRE(result.message.status == log::WriteStatus::RingFull);
        REQUIRE_FALSE(result.message.messageTruncated);
        REQUIRE(result.dump.status == log::WriteStatus::Written);
        REQUIRE(result.dump.messageTruncated);
        REQUIRE(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
        REQUIRE(log::lastWriteOutcome().messageTruncated);
    }
    SECTION("a refused dump cannot erase an admitted message's truncation") {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            return ++writes == 1;
        });
        result = log::infoHex(largeMessage, smallDump);
        REQUIRE(result.message.status == log::WriteStatus::Written);
        REQUIRE(result.message.messageTruncated);
        REQUIRE(result.dump.status == log::WriteStatus::RingFull);
        REQUIRE_FALSE(result.dump.messageTruncated);
        REQUIRE(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
        REQUIRE(log::lastWriteOutcome().messageTruncated);
    }
    SECTION("an admitted short dump cannot erase an admitted message's truncation") {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            ++writes;
            return true;
        });
        result = log::infoHex(largeMessage, smallDump);
        REQUIRE(result.message.messageTruncated);
        REQUIRE(result.dump.status == log::WriteStatus::Written);
        REQUIRE_FALSE(result.dump.messageTruncated);
        REQUIRE(log::lastWriteOutcome().status == log::WriteStatus::Written);
        REQUIRE(log::lastWriteOutcome().messageTruncated);
    }
    SECTION("refusing both oversized records reports no admitted truncation") {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            ++writes;
            return false;
        });
        result = log::infoHex(largeMessage, largeDump);
        REQUIRE(result.message.status == log::WriteStatus::RingFull);
        REQUIRE(result.dump.status == log::WriteStatus::RingFull);
        REQUIRE_FALSE(result.message.messageTruncated);
        REQUIRE_FALSE(result.dump.messageTruncated);
        REQUIRE(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
        REQUIRE_FALSE(log::lastWriteOutcome().messageTruncated);
    }
    REQUIRE(writes == 2);
}
