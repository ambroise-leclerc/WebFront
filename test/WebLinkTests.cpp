#include "InjectableNetworking.hpp"

#include <catch2/catch_test_macros.hpp>
#include <http/WebSocket.hpp>
#include <weblink/WebLink.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    WebLink<InjectableNetworking> link{InjectableSocket{}, 7, [](WebLinkEvent) {}};

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

SCENARIO("WebLink rejects incomplete asynchronous results") {
    InjectableSocket::reset();
    WebLink<InjectableNetworking> link{InjectableSocket{}, 8, [](WebLinkEvent) {}};

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
    WebLink<InjectableNetworking> link{InjectableSocket{}, 9, [&](WebLinkEvent event) {
                                           events.push_back(event.code);
                                           if (event.code == WebLinkEvent::Code::cppFunctionCalled) {
                                               calledName     = event.text;
                                               receivedCallId = event.callId;
                                               msg::FunctionCall<>::decodeParameter(calledValue, event.data);
                                           }
                                       }};

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

    GIVEN("a closed browser connection") {
        InjectableSocket::fail(make_error_code(errc::connection_reset));
        auto [callId, result] = link.expectResult<string>();

        THEN("new calls fail without allocating an identifier") {
            REQUIRE(callId == 0);
            REQUIRE(futureError(result) == "Browser connection closed");
        }
    }
}

SCENARIO("WebLink dispatches every WebSocket frame coalesced in one read") {
    InjectableSocket::reset();
    vector<WebLinkEvent::Code>    events;
    string                        calledName;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 10, [&](WebLinkEvent event) {
                                           events.push_back(event.code);
                                           if (event.code == WebLinkEvent::Code::cppFunctionCalled)
                                               calledName = event.text;
                                       }};

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
    WebLink<InjectableNetworking> link{InjectableSocket{}, 41, [](WebLinkEvent) {}};
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
    auto link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{31}, [](WebLinkEvent) {});
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
    link = make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{32}, [](WebLinkEvent) {});
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
    WebLink<InjectableNetworking> link{InjectableSocket{}, 51, [](WebLinkEvent) {}};
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
