#include <catch2/catch_test_macros.hpp>
#include <http/WebSocket.hpp>
#include <networking/NetworkingMock.hpp>
#include <weblink/WebLink.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

using namespace std;
using namespace webfront;

namespace {

class InjectableSocket : public networking::SocketBaseMock {
public:
    enum shutdown_type { shutdown_receive, shutdown_send, shutdown_both };

    void async_read_some(auto buffer, auto completion) {
        readBuffer  = buffer;
        readHandler = std::move(completion);
    }

    size_t write_some(auto input, error_code&) {
        const auto* first = static_cast<const byte*>(input.data());
        written.insert(written.end(), first, first + input.size());
        return input.size();
    }

    void close() {}
    void shutdown(shutdown_type) {}

    static void receive(span<const byte> bytes) {
        REQUIRE(readHandler);
        REQUIRE(bytes.size() <= readBuffer.size());
        copy(bytes.begin(), bytes.end(), static_cast<byte*>(readBuffer.data()));
        auto handler = std::move(readHandler);
        handler({}, bytes.size());
    }

    static void fail(error_code error) {
        REQUIRE(readHandler);
        auto handler = std::move(readHandler);
        handler(error, 0);
    }

    static void reset() {
        readHandler = {};
        written.clear();
    }

    static bool hasWrittenData() {
        return !written.empty();
    }

private:
    inline static networking::buffers::MutableBuffer readBuffer;
    inline static function<void(error_code, size_t)> readHandler;
    inline static vector<byte>                       written;
};

class InjectableNetworking : public networking::NetworkingMock {
public:
    using Socket = InjectableSocket;

    template <typename WriteHandler>
    static void AsyncWrite(Socket socket, auto buffers, WriteHandler handler) {
        error_code error;
        size_t     transferred = 0;
        for (const auto& buffer : buffers)
            transferred += socket.write_some(buffer, error);
        handler(error, transferred);
    }
};

vector<byte> clientFrame(span<const byte> payload) {
    REQUIRE(payload.size() < 126);
    constexpr array<byte, 4> mask{byte{0x12}, byte{0x34}, byte{0x56}, byte{0x78}};
    vector<byte>             frame{byte{0x82}, byte{static_cast<uint8_t>(0x80u | payload.size())}};
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (size_t index = 0; index < payload.size(); ++index)
        frame.push_back(payload[index] ^ mask[index % mask.size()]);
    return frame;
}

template <typename Net>
vector<byte> messagePayload(const websocket::Frame<Net>& frame) {
    vector<byte> payload;
    const auto   buffers = frame.toBuffers();
    for (auto buffer = next(buffers.begin()); buffer != buffers.end(); ++buffer) {
        const auto* first = static_cast<const byte*>(buffer->data());
        payload.insert(payload.end(), first, first + buffer->size());
    }
    return payload;
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
            REQUIRE(futureError(result) == "Browser connection closed: Connection reset by peer");
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
        call.encodeParameter(string{"registered"}, frame);
        call.encodeParameter(42, frame);
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
