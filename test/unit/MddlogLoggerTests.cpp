/** @brief Module-free consumer regressions for WebFront's optional diagnostic facade. */
#include <webfront/tooling/Logger.hpp>
#include "Bdd.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

using namespace webfront::bdd;

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

const Scenario mddlogFacade{"mddlog facade masks, caller location and independent registrations", {"mddlog", "logger"}, [] {
    namespace log = webfront::log;
    std::vector<std::string> lines;
    std::size_t              otherWrites = 0;
    LoggingScope             scope;
    for (const auto level : {log::Error, log::Warn, log::Info, log::Debug})
        require(!log::is(level));
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
    require(lines.size() == 1);
    require(lines[0].starts_with("[E]"));
    require(lines[0].ends_with(" | error 42"));
    log::setLogLevel(log::Debug);
    const auto callerLine = std::source_location::current().line() + 1;
    log::debug("answer {}", 42);
    require(lines.back().find(std::format("{:16}:{:4} |", "MddlogLoggerTests.cpp", callerLine)) != std::string::npos);
    log::info("info {}", 42);
    require(lines.back().starts_with("[I]"));
    require(!lines.back().contains("MddlogLoggerTests.cpp"));
    log::removeSinks(handles[0]);
    log::warn("remaining");
    require(lines.size() == 3);
    require(otherWrites == 4);
    log::removeSinks(handles[0]);
    log::set(log::Disabled, true);
    require(!log::is(log::Error));
    require(!log::is(log::Debug));
    log::info("invalid {");
    require(otherWrites == 4);
}};

const Scenario mddlogFacadeRenders{"mddlog facade renders every byte independently of char signedness", {"mddlog", "logger"}, [] {
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
    require(lines.size() == 2);
    require(lines[0].ends_with(" | all bytes {literal}"));
    require(lines[1].size() == (16 * 75) + 15);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto expected = index >= 0x20 && index <= 0x7E ? static_cast<char>(index) : '.';
        require(lines[1][((index / 16) * 76) + 59 + (index % 16)] == expected);
    }
    log::set(log::Info, false);
    log::infoHex("disabled", bytes);
    require(lines.size() == 2);
}};

const Scenario mddlogFacadePreserves{"mddlog facade preserves structured context and independent text delivery", {"mddlog", "logger"}, [] {
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
        require(record.component == "jsFunction");
        require(record.correlationId == "2");
        operations.emplace_back(record.operationId);
        return admit;
    });
    {
        const log::ContextScope context({.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::CppToJs, .callId = "1"});
        require(log::tryWrite(log::Info, "request").status == log::WriteStatus::Written);
        {
            const log::ContextScope nested({.component = "jsFunction", .webLinkId = "2", .direction = log::CallDirection::JsToCpp, .callId = "3"});
            log::info("reply");
        }
        admit             = false;
        const auto result = log::infoHex("bytes", std::array<unsigned char, 2>{0, 65});
        require(result.message.status == log::WriteStatus::RingFull);
        require(result.dump.status == log::WriteStatus::RingFull);
        require(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
    }
    require(operations == std::vector<std::string>{"cpp-js:1", "js-cpp:3", "cpp-js:1", "cpp-js:1"});
    require(lines.size() == 4);
    require(lines.back() == "00000000  00 41                                            .A");
    const std::string oversized(33, 'x');
    {
        const log::ContextScope context({.component = oversized, .webLinkId = "2", .direction = log::CallDirection::None, .callId = ""});
        const auto              result = log::tryWrite(log::Info, "refused context");
        require(result.status == log::WriteStatus::IdentifierTooLong);
        require(result.field == log::ContextField::Component);
    }
    require(operations.size() == 4);
    require(lines.size() == 5);
}};

const Scenario mddlogHex{"mddlog hex summary retains truncation from admitted records only", {"mddlog", "logger"}, [] {
    namespace log = webfront::log;
    const LoggingScope scope;
    log::setLogLevel(log::Info);
    const std::array<std::byte, 48> largeDump{};
    const std::array<std::byte, 1>  smallDump{};
    const std::string               largeMessage(161, 'm');
    unsigned int                    writes = 0;
    log::HexWriteOutcome            result;

    section("a refused message remains unmarked while an admitted dump is truncated", [&] {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            return ++writes != 1;
        });
        result = log::infoHex("refused message", largeDump);
        require(result.message.status == log::WriteStatus::RingFull);
        require(!result.message.messageTruncated);
        require(result.dump.status == log::WriteStatus::Written);
        require(result.dump.messageTruncated);
        require(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
        require(log::lastWriteOutcome().messageTruncated);
    });
    section("a refused dump cannot erase an admitted message's truncation", [&] {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            return ++writes == 1;
        });
        result = log::infoHex(largeMessage, smallDump);
        require(result.message.status == log::WriteStatus::Written);
        require(result.message.messageTruncated);
        require(result.dump.status == log::WriteStatus::RingFull);
        require(!result.dump.messageTruncated);
        require(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
        require(log::lastWriteOutcome().messageTruncated);
    });
    section("an admitted short dump cannot erase an admitted message's truncation", [&] {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            ++writes;
            return true;
        });
        result = log::infoHex(largeMessage, smallDump);
        require(result.message.messageTruncated);
        require(result.dump.status == log::WriteStatus::Written);
        require(!result.dump.messageTruncated);
        require(log::lastWriteOutcome().status == log::WriteStatus::Written);
        require(log::lastWriteOutcome().messageTruncated);
    });
    section("refusing both oversized records reports no admitted truncation", [&] {
        log::setRecordWriter([&](const log::DiagnosticRecord&) {
            ++writes;
            return false;
        });
        result = log::infoHex(largeMessage, largeDump);
        require(result.message.status == log::WriteStatus::RingFull);
        require(result.dump.status == log::WriteStatus::RingFull);
        require(!result.message.messageTruncated);
        require(!result.dump.messageTruncated);
        require(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
        require(!log::lastWriteOutcome().messageTruncated);
    });
    require(writes == 2);
}};
