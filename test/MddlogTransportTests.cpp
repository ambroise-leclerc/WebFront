/** @brief Bounded browser transport lane: context, saturation, failures, reentrance and WebLink lifecycle. */
#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <tooling/Logger.hpp>
#include <weblink/WebLink.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace std;
using namespace webfront;
using namespace webfront::testing;

namespace {

/** @brief Owned copy of a delivered record; the callback's views end with the callback. */
struct Delivered {
    string         text;
    string         message;
    string         component;
    string         operationId;
    string         correlationId;
    uint8_t        level     = 0;
    uint_least32_t line      = 0;
    bool           truncated = false;
};

/** @brief Thread-safe transport that copies every record it receives. */
class Collector {
public:
    void operator()(const log::TransportRecord& record) {
        lock_guard lock(guard);
        records.push_back({.text          = string(record.text),
                           .message       = string(record.message),
                           .component     = string(record.component),
                           .operationId   = string(record.operationId),
                           .correlationId = string(record.correlationId),
                           .level         = record.level,
                           .line          = record.location.line(),
                           .truncated     = record.messageTruncated});
    }
    [[nodiscard]] vector<Delivered> snapshot() {
        lock_guard lock(guard);
        return records;
    }

private:
    std::mutex        guard;
    vector<Delivered> records;
};

/**
 * @brief Drain, then remove every transport before the objects they capture are destroyed.
 * Declare it after those objects so that it is destroyed first.
 */
struct TransportScope {
    TransportScope() {
        log::setLogLevel(log::Info);
    }
    TransportScope(const TransportScope&)            = delete;
    TransportScope& operator=(const TransportScope&) = delete;
    TransportScope(TransportScope&&)                 = delete;
    TransportScope& operator=(TransportScope&&)      = delete;
    ~TransportScope() {
        log::flushTransports();
        for (const auto& handle : handles)
            log::removeTransport(handle);
        log::setLogLevel(log::Disabled);
    }
    log::TransportHandle add(function<void(const log::TransportRecord&)> write) {
        return handles.emplace_back(log::addTransport(std::move(write)));
    }
    vector<log::TransportHandle> handles;
};

/** @brief Blocks the consumer inside a transport; shared so that it outlives that transport. */
struct Gate {
    /** @brief Called by the transport: signal entry, then wait until opened. */
    void block() {
        entered.set_value();
        released.wait();
    }
    [[nodiscard]] bool waitEntered() {
        return enteredFuture.wait_for(5s) == future_status::ready;
    }
    void open() {
        if (!opened.exchange(true))
            release.set_value();
    }

private:
    promise<void>       entered;
    future<void>        enteredFuture = entered.get_future();
    promise<void>       release;
    shared_future<void> released = release.get_future().share();
    atomic<bool>        opened{false};
};

/** @brief Opens the gate on every exit path; declare after the TransportScope so its flush cannot hang. */
struct OpenOnExit {
    explicit OpenOnExit(shared_ptr<Gate> value) : gate(std::move(value)) {}
    OpenOnExit(const OpenOnExit&)            = delete;
    OpenOnExit& operator=(const OpenOnExit&) = delete;
    OpenOnExit(OpenOnExit&&)                 = delete;
    OpenOnExit& operator=(OpenOnExit&&)      = delete;
    ~OpenOnExit() {
        gate->open();
    }
    shared_ptr<Gate> gate;
};

/** @brief Counter differences across a test; cumulative counters survive previous tests. */
log::TransportHealth since(const log::TransportHealth& before) {
    const auto now = log::transportHealth();
    return {.delivered        = now.delivered - before.delivered,
            .writeFailures    = now.writeFailures - before.writeFailures,
            .reportedFailures = now.reportedFailures - before.reportedFailures,
            .detachments      = now.detachments - before.detachments,
            .reentrantRecords = now.reentrantRecords - before.reentrantRecords,
            .ringRefusals     = now.ringRefusals - before.ringRefusals,
            .ringUnavailable  = now.ringUnavailable - before.ringUnavailable,
            .drainFailures    = now.drainFailures - before.drainFailures,
            .activeTransports = now.activeTransports};
}

/** @brief Failure accounting compared as one value, printed when it differs. */
struct Failures {
    uint64_t        delivered                         = 0;
    uint64_t        writeFailures                     = 0;
    uint64_t        reportedFailures                  = 0;
    uint64_t        detachments                       = 0;
    bool            operator==(const Failures&) const = default;
    friend ostream& operator<<(ostream& out, const Failures& value) {
        return out << format("{{delivered {}, write failures {}, reported failures {}, detachments {}}}",
                             value.delivered,
                             value.writeFailures,
                             value.reportedFailures,
                             value.detachments);
    }
};

Failures failures(const log::TransportHealth& health) {
    return {.delivered        = health.delivered,
            .writeFailures    = health.writeFailures,
            .reportedFailures = health.reportedFailures,
            .detachments      = health.detachments};
}

/** @brief The captured context of one delivered record. */
struct Identity {
    string          component;
    string          operationId;
    string          correlationId;
    bool            operator==(const Identity&) const = default;
    friend ostream& operator<<(ostream& out, const Identity& value) {
        return out << format("{{{}, {}, {}}}", value.component, value.operationId, value.correlationId);
    }
};

vector<Identity> identities(const vector<Delivered>& records) {
    vector<Identity> result;
    for (const auto& record : records)
        result.push_back({record.component, record.operationId, record.correlationId});
    return result;
}

vector<string> deliveredMessages(const vector<Delivered>& records) {
    vector<string> result;
    for (const auto& record : records)
        result.push_back(record.message);
    return result;
}

void handshake() {
    msg::Handshake message;
    InjectableSocket::receive(clientFrame(span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())));
}

/** @brief A linked WebLink whose browser transport has already delivered one record. */
class LinkedBrowser {
public:
    LinkedBrowser() {
        InjectableSocket::reset();
        log::setLogLevel(log::Info);
        link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, 21, [](WebLinkEvent) {});
        handshake();
        log::info("delivered to the browser");
        REQUIRE(log::flushTransports());
        REQUIRE(InjectableSocket::wrote("delivered to the browser"));
        before = log::transportHealth();
        REQUIRE(before.activeTransports >= 1);
    }
    LinkedBrowser(const LinkedBrowser&)            = delete;
    LinkedBrowser& operator=(const LinkedBrowser&) = delete;
    LinkedBrowser(LinkedBrowser&&)                 = delete;
    LinkedBrowser& operator=(LinkedBrowser&&)      = delete;
    ~LinkedBrowser() {
        link.reset();
        log::setLogLevel(log::Disabled);
    }

    unique_ptr<WebLink<InjectableNetworking>> link;
    log::TransportHealth                      before;
};

void flushThrice() {
    for (int drain = 0; drain < 3; ++drain)
        REQUIRE(log::flushTransports());
}

void checkDebugRendering(const Delivered& record, uint_least32_t callerLine) {
    CHECK(record.text.starts_with("[D] "));
    CHECK(record.text.contains(format("{:16}:{:4} |", "MddlogTransportTests.cpp", callerLine)));
    CHECK(record.line == callerLine);
}

void checkTruncation(const Delivered& record) {
    CHECK(record.truncated);
    CHECK(record.message.size() == 160);
    CHECK(record.text.starts_with("[W] "));
}

}  // namespace

SCENARIO("Browser transports receive the context captured at emission", "[mddlog][transport]") {
    struct Event {
        string_view        component;
        string_view        link;
        log::CallDirection direction;
        string_view        call;
    };
    // ADR-003 Decision 6: two links, CallId 1 in both directions, then call 2 and disconnection.
    constexpr array<Event, 7> events{
        {{"jsFunction", "1", log::CallDirection::CppToJs, "1"},
         {"jsFunction", "2", log::CallDirection::CppToJs, "1"},
         {"jsFunction", "2", log::CallDirection::CppToJs, "1"},
         {"jsFunction", "1", log::CallDirection::CppToJs, "1"},
         {"cppFunction", "1", log::CallDirection::JsToCpp, "1"},
         {"jsFunction", "2", log::CallDirection::CppToJs, "2"},
         {"weblink", "2", log::CallDirection::CppToJs, "2"}}
    };
    Collector      first;
    Collector      second;
    TransportScope scope;
    scope.add(ref(first));
    scope.add(ref(second));

    vector<Identity> expected;
    for (const auto& event : events) {
        // The connection's strings die before the drain; transports must not need them.
        string            component(event.component);
        string            link(event.link);
        string            call(event.call);
        log::ContextScope context({.component = component, .webLinkId = link, .direction = event.direction, .callId = call});
        REQUIRE(log::tryWrite(log::Info, "identical text with misleading ids 999").status == log::WriteStatus::Written);
        const auto prefix = event.direction == log::CallDirection::CppToJs ? "cpp-js:" : "js-cpp:";
        expected.push_back({component, prefix + call, link});
    }
    REQUIRE(log::flushTransports());

    const auto delivered = first.snapshot();
    CHECK(identities(delivered) == expected);
    CHECK(identities(second.snapshot()) == expected);
    CHECK(ranges::all_of(delivered, [](const Delivered& record) {
        return record.level == log::Info && record.text.starts_with("[I] ") && record.text.ends_with(" | identical text with misleading ids 999");
    }));
}

SCENARIO("Browser transports keep the legacy rendering and the governed truncation flag", "[mddlog][transport]") {
    Collector      collector;
    TransportScope scope;
    scope.add(ref(collector));
    log::setLogLevel(log::Debug);

    const auto callerLine = source_location::current().line() + 1;
    log::debug("located {}", 1);
    log::infoHex("frame:", array<uint8_t, 2>{0x41, 0x7f});
    REQUIRE(log::tryWrite(log::Warn, string(200, 'x')).status == log::WriteStatus::Written);
    REQUIRE(log::lastWriteOutcome().messageTruncated);
    log::error("outside any call");
    REQUIRE(log::flushTransports());

    const auto records = collector.snapshot();
    REQUIRE(records.size() == 5);
    checkDebugRendering(records[0], callerLine);
    CHECK(records[1].text.ends_with(" | frame:"));
    CHECK(records[2].message.ends_with(" A."));
    checkTruncation(records[3]);
    CHECK(records[4].text.starts_with("[E] "));
    CHECK(identities({records[4]}) == vector<Identity>{{}});
}

SCENARIO("A blocked transport never blocks producers; saturation is refused observably", "[mddlog][transport]") {
    const auto     gate = make_shared<Gate>();
    atomic<size_t> calls{0};
    TransportScope scope;
    OpenOnExit     opener(gate);
    scope.add([&calls, gate](const log::TransportRecord&) {
        if (calls.fetch_add(1) == 0)
            gate->block();
    });
    const auto before = log::transportHealth();

    log::info("blocks the consumer");
    REQUIRE(gate->waitEntered());
    constexpr size_t overflow = 20;
    size_t           written  = 0;
    size_t           refused  = 0;
    for (size_t index = 0; index < log::transportRingCapacity + overflow; ++index) {
        const auto status = log::tryWrite(log::Info, "while blocked").status;
        written          += status == log::WriteStatus::Written ? 1 : 0;
        refused          += status == log::WriteStatus::RingFull ? 1 : 0;
    }
    CHECK(pair(written, refused) == pair(log::transportRingCapacity, overflow));
    CHECK(log::lastWriteOutcome().status == log::WriteStatus::RingFull);
    CHECK(since(before).ringRefusals == overflow);

    gate->open();
    REQUIRE(log::flushTransports());
    CHECK(calls.load() == 1 + log::transportRingCapacity);
    CHECK(since(before).delivered == 1 + log::transportRingCapacity);
}

SCENARIO("A transport throwing synchronously is detached before its next record", "[mddlog][transport]") {
    atomic<size_t> calls{0};
    Collector      healthy;
    TransportScope scope;
    scope.add([&](const log::TransportRecord&) {
        ++calls;
        throw runtime_error("socket gone");
    });
    scope.add(ref(healthy));
    const auto before = log::transportHealth();

    log::info("first");
    log::info("second");
    REQUIRE(log::flushTransports());
    log::info("third");
    REQUIRE(log::flushTransports());

    const auto health = since(before);
    CHECK(calls.load() == 1);
    CHECK(failures(health) == Failures{.delivered = 3, .writeFailures = 1, .reportedFailures = 0, .detachments = 1});
    CHECK(health.activeTransports == before.activeTransports - 1);
}

SCENARIO("Logging synchronously from a transport is not amplified", "[mddlog][transport]") {
    atomic<size_t> echoes{0};
    Collector      observer;
    TransportScope scope;
    scope.add([&](const log::TransportRecord& record) {
        ++echoes;
        log::error("echo of {}", record.message);
    });
    scope.add(ref(observer));
    const auto before = log::transportHealth();

    log::info("origin");
    flushThrice();

    CHECK(echoes.load() == 1);
    CHECK(deliveredMessages(observer.snapshot()) == vector<string>{"origin"});
    CHECK(since(before).reentrantRecords == 1);
}

SCENARIO("An asynchronous write failure detaches the transport before its diagnostic", "[mddlog][transport]") {
    mutex          queuedMutex;
    vector<string> queued;
    Collector      observer;
    TransportScope scope;
    const auto     failing = scope.add([&](const log::TransportRecord& record) {
        lock_guard lock(queuedMutex);
        queued.emplace_back(record.message);
    });
    scope.add(ref(observer));
    const auto before = log::transportHealth();

    log::info("queued write");
    REQUIRE(log::flushTransports());
    // The write completes later, on another thread, which reports the failure first.
    thread([&] {
        log::reportTransportFailure(failing);
        log::error("browser write failed");
    }).join();
    flushThrice();

    CHECK(queued == vector<string>{"queued write"});
    CHECK(observer.snapshot().size() == 2);
    CHECK(failures(since(before)) == Failures{.delivered = 3, .writeFailures = 0, .reportedFailures = 1, .detachments = 1});
}

SCENARIO("Removal during emission is quiescent, so captures can be destroyed at once", "[mddlog][transport]") {
    log::setLogLevel(log::Info);
    for (int round = 0; round < 10; ++round) {
        auto           state = make_unique<vector<int>>();
        atomic<bool>   removed{false};
        atomic<int>    late{0};
        auto           handle = log::addTransport([&removed, &late, raw = state.get()](const log::TransportRecord&) {
            if (removed.load())
                ++late;
            raw->push_back(1);
        });
        atomic<bool>   stop{false};
        vector<thread> producers;
        for (int producer = 0; producer < 3; ++producer)
            producers.emplace_back([&stop] {
                while (!stop.load())
                    log::info("load");
            });
        REQUIRE(log::flushTransports());
        log::removeTransport(handle);
        removed.store(true);
        state.reset();
        REQUIRE(log::flushTransports());
        stop.store(true);
        for (auto& producer : producers)
            producer.join();
        CHECK(late.load() == 0);
    }
    log::setLogLevel(log::Disabled);
}

SCENARIO("WebLink removes its browser transport on normal disconnection", "[mddlog][transport][weblink]") {
    LinkedBrowser browser;
    InjectableSocket::fail(make_error_code(errc::connection_reset));
    log::info("after disconnection");
    REQUIRE(log::flushTransports());

    const auto health = since(browser.before);
    CHECK(health.activeTransports == browser.before.activeTransports - 1);
    CHECK(health.detachments == 0);
    CHECK(!InjectableSocket::wrote("after disconnection"));
}

SCENARIO("WebLink captures the link and call context of a C++ function call", "[mddlog][transport][weblink]") {
    LinkedBrowser  browser;
    Collector      observer;
    TransportScope scope;
    scope.add(ref(observer));
    msg::FunctionCall<> call;
    call.setCallId(37);
    websocket::Frame<InjectableNetworking> frame{span(reinterpret_cast<const byte*>(call.header().data()), call.header().size())};
    const string                           functionName{"registered"};  // The frame borrows it until freeze() copies it.
    call.encodeParameter(functionName, frame);
    frame.freeze();
    InjectableSocket::receive(clientFrame(messagePayload(frame)));
    REQUIRE(log::flushTransports());

    // Received message and dump, then the call itself.
    const auto records = observer.snapshot();
    CHECK(identities(records)
          == vector<Identity>{
              {    "weblink",          "", "21"},
              {    "weblink",          "", "21"},
              {"cppFunction", "js-cpp:37", "21"}
    });
    CHECK(deliveredMessages(records).back() == "Function called !");
    CHECK(InjectableSocket::wrote("Function called !"));
}

SCENARIO("WebLink reports a write failing inside its transport before the diagnostic", "[mddlog][transport][weblink]") {
    LinkedBrowser  browser;
    Collector      observer;
    TransportScope scope;
    scope.add(ref(observer));
    InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
    log::info("cannot be sent");
    flushThrice();

    const auto health = since(browser.before);
    CHECK(failures(health) == Failures{.delivered = 2, .writeFailures = 0, .reportedFailures = 1, .detachments = 1});
    // The write-error diagnostic is logged during dispatch: suppressed for every transport.
    CHECK(health.reentrantRecords >= 1);
    CHECK(deliveredMessages(observer.snapshot()) == vector<string>{"cannot be sent"});
}

SCENARIO("WebLink reports a write failing on asynchronous completion", "[mddlog][transport][weblink]") {
    LinkedBrowser browser;
    InjectableSocket::deferWrites();
    InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
    log::info("completes later");
    REQUIRE(log::flushTransports());
    REQUIRE(InjectableSocket::completeDeferredWrites() == 1);
    flushThrice();

    // Neither the failure diagnostic nor anything later reaches the failed transport.
    const auto health = since(browser.before);
    CHECK(failures(health) == Failures{.delivered = 1, .writeFailures = 0, .reportedFailures = 1, .detachments = 1});
    CHECK(health.activeTransports == browser.before.activeTransports - 1);
}

SCENARIO("A WebLink destroyed before the drain receives nothing more", "[mddlog][transport][weblink]") {
    LinkedBrowser  browser;
    const auto     gate = make_shared<Gate>();
    atomic<int>    blocked{0};
    TransportScope scope;
    OpenOnExit     opener(gate);
    scope.add([&blocked, gate](const log::TransportRecord&) {
        if (blocked.fetch_add(1) == 0)
            gate->block();
    });
    log::info("holds the consumer");
    REQUIRE(gate->waitEntered());
    log::info("queued before destruction");
    browser.link.reset();
    gate->open();
    REQUIRE(log::flushTransports());

    CHECK(!InjectableSocket::wrote("queued before destruction"));
    // The link's transport left; the blocker is still registered.
    CHECK(since(browser.before).activeTransports == browser.before.activeTransports);
}

SCENARIO("A write failing after its WebLink is destroyed does not reach the link or its successor", "[mddlog][transport][weblink]") {
    InjectableSocket::reset();
    log::setLogLevel(log::Info);
    auto link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, 31, [](WebLinkEvent) {});
    handshake();
    // The old link's write completes only after the link is gone, keeping its WebSocket alive.
    InjectableSocket::deferWrites();
    InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
    log::info("written by the old link");
    REQUIRE(log::flushTransports());
    link.reset();
    InjectableSocket::failWrites({});
    InjectableSocket::deferWrites(false);

    // A new link, possibly at the same address, attaches its own transport.
    link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, 32, [](WebLinkEvent) {});
    handshake();
    const auto before = log::transportHealth();
    REQUIRE(InjectableSocket::completeDeferredWrites() == 1);
    log::info("delivered by the new link");
    REQUIRE(log::flushTransports());

    const auto health = since(before);
    CHECK(health.reportedFailures == 0);
    CHECK(health.activeTransports == before.activeTransports);
    CHECK(InjectableSocket::wrote("delivered by the new link"));
    link.reset();
    log::setLogLevel(log::Disabled);
}
