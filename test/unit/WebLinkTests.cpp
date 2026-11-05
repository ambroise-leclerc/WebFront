#include "InjectableNetworking.hpp"

#include <catch2/catch_test_macros.hpp>
#include <webfront/http/WebSocket.hpp>
#include <webfront/weblink/WebLink.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <future>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

using namespace std;
using namespace webfront;
using namespace webfront::testing;

namespace {

/// With mddlog, browser logs are delivered by the transport consumer thread; wait for it.
void drainBrowserLogs() {
#if defined(WEBFRONT_USE_MDDLOG) && WEBFRONT_USE_MDDLOG
    REQUIRE(log::flushTransports());
#endif
}

void browserHandshake() {
    msg::Handshake message;
    InjectableSocket::receive(clientFrame(span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())));
}

/// Runs its cleanup once, on request or at scope exit: a failing REQUIRE must not leave a sink that points into the test.
struct LogRestore {
    std::function<void()> cleanup;
    ~LogRestore() { run(); }
    void run() {
        if (auto once = std::move(cleanup); once) { cleanup = nullptr; once(); }
    }
};

template <typename Result>
string futureError(future<Result>& result) {
    try {
        if constexpr (is_void_v<Result>)
            result.get();
        else
            static_cast<void>(result.get());
    } catch (const exception& error) {
        return error.what();
    }
    FAIL("The asynchronous result did not throw");
    return {};
}

template <typename Configure>
vector<byte> returnMessage(msg::CallId callId, Configure configure) {
    msg::FunctionReturn<> message;
    message.setCallId(callId);
    websocket::Frame<InjectableNetworking> frame{span(reinterpret_cast<const byte*>(message.header().data()), message.header().size())};
    configure(message, frame);

    return clientFrame(messagePayload(frame));
}

}  // namespace

SCENARIO("WebLink settles correlated asynchronous results") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 7, [](WebLinkEvent) { }, scope};

    GIVEN("a pending string result") {
        auto [callId, result] = link.expectResult<string>();

        WHEN("the browser returns a matching value") {
            auto response = returnMessage(callId, [](auto& message, auto& frame) {
                const string value{"done"};
                message.encodeParameter(value, frame);
                frame.freeze();
            });
            InjectableSocket::receive(response);

            THEN("the future receives the decoded value") {
                REQUIRE(result.get() == "done");
            }
        }

        WHEN("the browser returns an exception") {
            auto response = returnMessage(callId, [](auto& message, auto& frame) {
                const runtime_error error{"JavaScript failed"};
                message.encodeParameter(error, frame);
                frame.freeze();
            });
            InjectableSocket::receive(response);

            THEN("the future rethrows it with its message") {
                REQUIRE(futureError(result) == "JavaScript failed");
            }
        }
    }

    GIVEN("a pending void result") {
        auto [callId, result] = link.expectResult<void>();

        WHEN("the browser returns no value") {
            auto response = returnMessage(callId, [](auto&, auto&) {});
            InjectableSocket::receive(response);

            THEN("the future completes") {
                REQUIRE_NOTHROW(result.get());
            }
        }
    }
}

namespace {
/// The BridgeError a future rethrows, as its code and message.
template <typename Result>
pair<string, string> futureFailure(future<Result>& result) {
    try {
        static_cast<void>(result.get());
    } catch (const BridgeError& error) {
        return {error.code(), error.what()};
    } catch (const exception& error) {
        FAIL("The failure is not a BridgeError: " << error.what());
    }
    FAIL("The asynchronous result did not throw");
    return {};
}
}  // namespace

SCENARIO("A browser rejection reaches C++ with its code") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 7, [](WebLinkEvent) { }, scope};

    GIVEN("a pending call the browser rejects with an application code") {
        auto [callId, result] = link.expectResult<string>();
        InjectableSocket::receive(returnMessage(callId, [](auto& message, auto& frame) {
            const BridgeError error{"denied", "the user said no"};
            message.encodeParameter(error, frame);
            frame.freeze();
        }));
        THEN("the future throws a BridgeError with that code and message") {
            REQUIRE(futureFailure(result) == pair<string, string>{"denied", "the user said no"});
        }
    }
    GIVEN("a pending call the browser rejects with a plain error") {
        auto [callId, result] = link.expectResult<string>();
        InjectableSocket::receive(returnMessage(callId, [](auto& message, auto& frame) {
            const runtime_error error{"plain failure"};
            message.encodeParameter(error, frame);
            frame.freeze();
        }));
        THEN("the code is the generic one") {
            REQUIRE(futureFailure(result) == pair<string, string>{string(bridge_code::generic), "plain failure"});
        }
    }
    GIVEN("a pending call the browser rejects with a code of the bridge") {
        auto [callId, result] = link.expectResult<void>();
        InjectableSocket::receive(returnMessage(callId, [](auto& message, auto& frame) {
            const auto error = detail::makeBridgeError(bridge_code::notFound, "JavaScript function 'x' was not found");
            message.encodeParameter(error, frame);
            frame.freeze();
        }));
        THEN("the reserved code is kept, which tells it from an application's") {
            const auto [code, text] = futureFailure(result);
            REQUIRE(code == bridge_code::notFound);
            REQUIRE(BridgeError::isReserved(code));
        }
    }
    GIVEN("a pending call whose result is not of the expected type") {
        auto [callId, result] = link.expectResult<double>();
        InjectableSocket::receive(returnMessage(callId, [](auto& message, auto& frame) {
            const string text{"text"};  // the frame borrows it until freeze()
            message.encodeParameter(text, frame);
            frame.freeze();
        }));
        THEN("the failure has the malformed code") {
            REQUIRE(futureFailure(result).first == bridge_code::malformed);
        }
    }
    GIVEN("a call pending when the connection closes") {
        auto [callId, result] = link.expectResult<string>();
        static_cast<void>(callId);
        InjectableSocket::fail(make_error_code(errc::connection_reset));
        THEN("the failure has the disconnected code") {
            REQUIRE(futureFailure(result).first == bridge_code::disconnected);
        }
    }
}

SCENARIO("WebLink rejects incomplete asynchronous results") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 8, [](WebLinkEvent) { }, scope};

    GIVEN("a pending value whose return is malformed") {
        auto [callId, result] = link.expectResult<string>();
        auto response         = returnMessage(callId, [](auto& message, auto&) {
            message.setParametersCount(1);
        });
        InjectableSocket::receive(response);

        THEN("the future receives the decoding error") {
            REQUIRE(futureError(result) == "Not enough data for msg::FunctionCall::decodeParameter");
        }
    }

    GIVEN("a pending value when the browser disconnects") {
        auto [callId, result] = link.expectResult<string>();
        static_cast<void>(callId);
        InjectableSocket::fail(make_error_code(errc::connection_reset));

        THEN("the future receives a connection error") {
            REQUIRE(futureError(result).starts_with("Browser connection closed:"));
        }
    }
}

SCENARIO("WebLink dispatches browser messages and allocates distinct calls") {
    InjectableSocket::reset();
    vector<WebLinkEvent::Code>    events;
    string                        calledName;
    int                           calledValue{};
    msg::CallId                   receivedCallId{};
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{},
                                       9,
                                       [&](WebLinkEvent event) {
                                           events.push_back(event.code);
                                           if (event.code == WebLinkEvent::Code::cppFunctionCalled) {
                                               calledName     = event.text;
                                               receivedCallId = event.callId;
                                               msg::FunctionCall<>::decodeParameter(calledValue, event.data);
                                           }
                                       },
                                       scope};

    GIVEN("a browser handshake and function call") {
        msg::Handshake handshake;
        auto           handshakePayload = span(reinterpret_cast<const byte*>(handshake.header().data()), handshake.header().size());
        InjectableSocket::receive(clientFrame(handshakePayload));

        msg::FunctionCall<> call;
        call.setCallId(37);
        websocket::Frame<InjectableNetworking> frame{span(reinterpret_cast<const byte*>(call.header().data()), call.header().size())};
        const string functionName{"registered"};
        call.encodeParameter(functionName, frame);
        call.encodeParameter(42, frame);
        frame.freeze();
        InjectableSocket::receive(clientFrame(messagePayload(frame)));

        THEN("the link acknowledges and dispatches the correlated call") {
            REQUIRE(events == vector{WebLinkEvent::Code::linked, WebLinkEvent::Code::cppFunctionCalled});
            REQUIRE(calledName == "registered");
            REQUIRE(calledValue == 42);
            REQUIRE(receivedCallId == 37);
            REQUIRE(InjectableSocket::hasWrittenData());
        }
    }

    GIVEN("multiple pending calls") {
        auto [firstId, first]   = link.expectResult<string>();
        auto [secondId, second] = link.expectResult<string>();
        static_cast<void>(first);
        static_cast<void>(second);

        THEN("their nonzero correlation identifiers are distinct") {
            REQUIRE(firstId != 0);
            REQUIRE(secondId != 0);
            REQUIRE(firstId != secondId);
        }
    }

    GIVEN("an explicit error response") {
        link.sendError(12, "not available");

        THEN("the encoded exception is written") {
            REQUIRE(InjectableSocket::hasWrittenData());
        }
    }

    GIVEN("an application error with a code, and one that tries to pass for the bridge's") {
        link.sendError(15, "no space", "quota:exceeded");
        link.sendError(16, "forged", "webfront.notFound");
        link.sendError(17, "ill formed", "bad code");

        THEN("the application code is kept and the others are sent as the generic one") {
            REQUIRE(InjectableSocket::wrote("quota:exceeded"));
            REQUIRE_FALSE(InjectableSocket::wrote(std::string(bridge_code::notFound)));
            REQUIRE_FALSE(InjectableSocket::wrote("bad code"));
            REQUIRE(InjectableSocket::wrote(std::string(bridge_code::generic)));
        }
    }

    GIVEN("a closed browser connection") {
        InjectableSocket::fail(make_error_code(errc::connection_reset));
        auto [callId, result] = link.expectResult<string>();

        THEN("new calls fail without allocating an identifier") {
            REQUIRE(callId == 0);
            REQUIRE(futureError(result) == "Browser connection closed");
        }
        THEN("the link reports the disconnection once") {
            REQUIRE(events == vector{WebLinkEvent::Code::closed});
        }
    }
}

SCENARIO("WebLink reports a browser's close frame as its disconnection") {
    InjectableSocket::reset();
    vector<WebLinkEvent::Code>    events;
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 10, [&](WebLinkEvent event) { events.push_back(event.code); }, scope};
    WHEN("the browser sends a close frame") {
        InjectableSocket::receive(std::array{byte{0x88}, byte{0x80}, byte{1}, byte{2}, byte{3}, byte{4}});
        THEN("the link reports one disconnection, and none when it is destroyed") {
            REQUIRE(events == vector{WebLinkEvent::Code::closed});
        }
    }
}

SCENARIO("WebLink dispatches every WebSocket frame coalesced in one read") {
    InjectableSocket::reset();
    vector<WebLinkEvent::Code>    events;
    string                        calledName;
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{},
                                       10,
                                       [&](WebLinkEvent event) {
                                           events.push_back(event.code);
                                           if (event.code == WebLinkEvent::Code::cppFunctionCalled)
                                               calledName = event.text;
                                       },
                                       scope};

    GIVEN("A handshake and function call delivered together") {
        msg::Handshake handshake;
        const auto     handshakePayload = span(reinterpret_cast<const byte*>(handshake.header().data()), handshake.header().size());
        auto           received         = clientFrame(handshakePayload);

        msg::FunctionCall<>                    call;
        websocket::Frame<InjectableNetworking> frame{span(reinterpret_cast<const byte*>(call.header().data()), call.header().size())};
        const string                           functionName{"registered"};
        call.encodeParameter(functionName, frame);
        frame.freeze();
        const auto callFrame = clientFrame(messagePayload(frame));
        received.insert(received.end(), callFrame.begin(), callFrame.end());

        WHEN("The socket completes one read") {
            InjectableSocket::receive(received);

            THEN("Both frames are dispatched in order") {
                REQUIRE(events == vector{WebLinkEvent::Code::linked, WebLinkEvent::Code::cppFunctionCalled});
                REQUIRE(calledName == "registered");
            }
        }
    }
}

SCENARIO("WebLink forwards logs to the browser until a write fails") {
    InjectableSocket::reset();
    log::setLogLevel(log::Info);
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 41, [](WebLinkEvent) { }, scope};
    browserHandshake();
    log::info("forwarded to the browser");
    drainBrowserLogs();
    REQUIRE(InjectableSocket::wrote("forwarded to the browser"));

    InjectableSocket::receive(clientFrame({}));  // An empty message is reported, not dispatched.
    InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
    log::info("lost with the connection");
    drainBrowserLogs();
    InjectableSocket::failWrites({});
    log::info("after the failure");
    drainBrowserLogs();

    // The failing write detached the transport before its diagnostic.
    CHECK(!InjectableSocket::wrote("after the failure"));
    log::setLogLevel(log::Disabled);
}

SCENARIO("A write failing after its WebLink is destroyed does not reach the link or its successor") {
    InjectableSocket::reset();
    log::setLogLevel(log::Info);
    InlineTaskScope scope;
    auto            link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{31}, [](WebLinkEvent) { }, scope);
    browserHandshake();
    // The old link's write completes only after the link is gone, keeping its WebSocket alive.
    InjectableSocket::deferWrites();
    InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
    log::info("written by the old link");
    drainBrowserLogs();
    link.reset();
    InjectableSocket::failWrites({});
    InjectableSocket::deferWrites(false);

    // A new link, possibly at the same address, attaches its own transport.
    link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{32}, [](WebLinkEvent) { }, scope);
    browserHandshake();
    REQUIRE(InjectableSocket::completeDeferredWrites() == 1);
    log::info("delivered by the new link");
    drainBrowserLogs();

    CHECK(InjectableSocket::wrote("delivered by the new link"));
    link.reset();
    log::setLogLevel(log::Disabled);
}

SCENARIO("A browser that stops reading bounds the diagnostic frames queued for it") {
    InjectableSocket::reset();
    log::setLogLevel(log::Info);
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 51, [](WebLinkEvent) { }, scope};
    browserHandshake();
    // Only the first write reaches the network, and it does not complete.
    InjectableSocket::deferWrites();
#if defined(WEBFRONT_USE_MDDLOG) && WEBFRONT_USE_MDDLOG
    const auto before = log::transportHealth();
#endif
    constexpr size_t messages = 10240;
    for (size_t index = 0; index < messages; ++index) {
        log::info("unread diagnostic");
        if (index % 64 == 63) drainBrowserLogs();  // Keep the rings below saturation.
    }
    drainBrowserLogs();

    CHECK(link.pendingWrites() == link.maxPendingLogFrames);
    CHECK(link.droppedLogFrames() == messages - link.maxPendingLogFrames);
#if defined(WEBFRONT_USE_MDDLOG) && WEBFRONT_USE_MDDLOG
    CHECK(log::transportHealth().overflows - before.overflows == link.droppedLogFrames());
#endif

    // Once the network completes, the queue drains and the link keeps working.
    InjectableSocket::deferWrites(false);
    REQUIRE(InjectableSocket::completeDeferredWrites() == 1);
    CHECK(link.pendingWrites() == 0);
    log::setLogLevel(log::Disabled);
}

namespace {
template <typename Writes>
networking::Task waitWrites(Writes writes, bool& woken) {
    co_await std::move(writes);
    woken = true;
}

// Records the events the link had reported when the writer resumed.
template <typename Writes>
networking::Task waitWritesSeeing(Writes writes, const vector<WebLinkEvent::Code>& events, vector<WebLinkEvent::Code>& seen) {
    co_await std::move(writes);
    seen = events;
}
}  // namespace

SCENARIO("A link wakes a writer once its write queue drains") {
    InjectableSocket::reset();
    InlineTaskScope scope;
    auto            link = make_shared<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{12}, [](WebLinkEvent) { }, scope);
    InjectableSocket::deferWrites();
    for (msg::CallId call = 1; call <= 3; ++call)
        link->sendError(call, "queued");
    REQUIRE(link->pendingWrites() == 3);
    using Writes = webfront::detail::WritesAtMost<WebLink<InjectableNetworking>>;
    bool woken = false;
    GIVEN("a task waiting for at most one queued frame") {
        networking::Spawn(scope, networking::ex::inline_scheduler{}, waitWrites(Writes{link, 1}, woken));
        THEN("it wakes when the second write ends, not before") {
            CHECK_FALSE(woken);
            InjectableSocket::completeDeferredWrites();
            CHECK_FALSE(woken);
            InjectableSocket::completeDeferredWrites();
            CHECK(woken);
        }
    }
    GIVEN("a waiting task that is stopped") {
        networking::ex::inplace_stop_source stop;
        networking::Spawn(scope, networking::ex::inline_scheduler{}, stop.get_token(), waitWrites(Writes{link, 0}, woken));
        stop.request_stop();
        THEN("it unwinds, and the drained queue does not call it") {
            while (InjectableSocket::completeDeferredWrites() > 0) { }
            CHECK_FALSE(woken);
            CHECK(link->pendingWrites() == 0);
        }
    }
    GIVEN("a link destroyed while a task waits") {
        networking::Spawn(scope, networking::ex::inline_scheduler{}, waitWrites(Writes{link, 0}, woken));
        link.reset();
        THEN("the task resumes: nothing is left to wait for") {
            CHECK(woken);
        }
    }
    GIVEN("a link that is gone") {
        std::weak_ptr<WebLink<InjectableNetworking>> gone;
        networking::Spawn(scope, networking::ex::inline_scheduler{}, waitWrites(Writes{gone, 0}, woken));
        THEN("the task does not wait") {
            CHECK(woken);
        }
    }
    InjectableSocket::deferWrites(false);
    while (InjectableSocket::completeDeferredWrites() > 0) { }
}

SCENARIO("A writer resumes after a failed write reported the closure") {
    InjectableSocket::reset();
    InlineTaskScope            scope;
    vector<WebLinkEvent::Code> events;
    auto link = make_shared<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{13}, [&events](WebLinkEvent event) { events.push_back(event.code); }, scope);
    InjectableSocket::deferWrites();
    for (msg::CallId call = 1; call <= 3; ++call)
        link->sendError(call, "queued");
    vector<WebLinkEvent::Code> seen{WebLinkEvent::Code::linked};  // replaced when the writer resumes
    networking::Spawn(scope, networking::ex::inline_scheduler{}, waitWritesSeeing(webfront::detail::WritesAtMost<WebLink<InjectableNetworking>>{link, 0}, events, seen));
    WHEN("the write fails") {
        InjectableSocket::failWrites(make_error_code(errc::broken_pipe));
        InjectableSocket::deferWrites(false);
        InjectableSocket::completeDeferredWrites();
        THEN("the writer resumes once the closure was reported") {
            CHECK(seen == vector{WebLinkEvent::Code::closed});
        }
    }
    InjectableSocket::reset();
}

SCENARIO("WebLink never logs the payload of an incoming message") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 7, [](WebLinkEvent) { }, scope};
    auto [callId, result] = link.expectResult<string>();

    log::setLogLevel(log::Info);
    string     logged;
    std::mutex loggedMutex;
    const auto sink = log::addSinks([&](string_view line) {
        std::scoped_lock lock{loggedMutex};
        logged += string(line) + "\n";
    });
    LogRestore restore_sink{[&] { log::removeSinks(sink); log::setLogLevel(log::Error); }};
    auto response = returnMessage(callId, [](auto& message, auto& frame) {
        const string value{"/webfront/services/files/0123456789abcdef.QQQQQQQQQQQQQQQQ/a.txt"};
        message.encodeParameter(value, frame);
        frame.freeze();
    });
    InjectableSocket::receive(response);
    REQUIRE(result.get().starts_with("/webfront/services/files/"));
    restore_sink.run();
    std::scoped_lock lock{loggedMutex};
    REQUIRE(logged.find("QQQQ") == string::npos);
    REQUIRE(logged.find("51 51") == string::npos);
}
