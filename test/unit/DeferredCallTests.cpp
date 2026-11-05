// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief C++ callbacks that answer a JavaScript call later: their result, failures, stop, disconnection and shutdown (#459).
#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/WebFront.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace webfront;
using Socket = testing::InjectableSocket;

namespace {

const std::string upgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

struct OneBrowserNet : testing::InjectableNetworking {
    static inline bool accepted{};
    static auto        Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (std::exchange(accepted, true))
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

using App = BasicWF<OneBrowserNet, fs::DefaultFS>;

/// The operations the test completes by hand, by the name of the call that awaits them.
struct Gates {
    std::vector<testing::ManualSender::Completion> open;
    void                                           release(std::size_t index) { open.at(index).complete({}, 0); }
    void                                           stopOne(std::size_t index) { open.at(index).stop(); }
};
Gates gates;

testing::ManualSender gate() {
    return testing::ManualSender([](testing::ManualSender::Completion completion) { gates.open.push_back(std::move(completion)); });
}

ResultTask<std::string> echoLater(std::string value) {
    co_await gate();
    co_return "answered:" + value;
}

ResultTask<std::string> failLater(std::string) {
    co_await gate();
    throw BridgeError("quota:exceeded", "the disk is full");
}

ResultTask<void> voidLater() {
    co_await gate();
}

ResultTask<std::string> watchedLater(std::weak_ptr<int>* observed, std::shared_ptr<int> lifetime) {
    *observed = lifetime;
    co_await gate();
    co_return "late";
}

/// A call from the browser: the function, its identifier and one string argument.
std::vector<std::byte> callMessage(const std::string& name, msg::CallId callId, const std::string& argument = "") {
    msg::FunctionCall<> message;
    message.setCallId(callId);
    websocket::Frame<OneBrowserNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(name, frame);
    if (!argument.empty())
        message.encodeParameter(argument, frame);
    frame.freeze();
    return testing::clientFrame(testing::messagePayload(frame));
}

/// Resets the shared mock socket before the application accepts its connection.
struct Fresh {
    Fresh() {
        Socket::reset();
        gates.open.clear();
        OneBrowserNet::accepted = false;
    }
};

struct Browser {
    Fresh fresh;
    App   app{WebFrontConfig{.port = "0"}};

    void connect() {
        Socket::receive(std::as_bytes(std::span(upgradeRequest)));
        msg::Handshake handshake;
        Socket::receive(testing::clientFrame(std::span(reinterpret_cast<const std::byte*>(handshake.header().data()), handshake.header().size())));
    }
    void call(const std::string& name, msg::CallId callId, const std::string& argument = "") {
        Socket::receive(callMessage(name, callId, argument));
    }
    ~Browser() {
        app.shutdown();
        Socket::reset();
        gates.open.clear();
    }
};

}  // namespace

SCENARIO("A callback that returns a task answers when the task completes", "[deferred]") {
    Browser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();

    WHEN("the browser calls it") {
        browser.call("echoLater", 5, "x");
        THEN("nothing is answered while the task is suspended") {
            REQUIRE(gates.open.size() == 1);
            REQUIRE_FALSE(Socket::wrote("answered:x"));
        }
        AND_WHEN("the task completes") {
            gates.release(0);
            THEN("its value is sent as the result of the call") {
                REQUIRE(Socket::wrote("answered:x"));
            }
        }
    }
    WHEN("the call asks for no answer") {
        browser.call("echoLater", 0, "x");
        gates.release(0);
        THEN("nothing is sent") {
            REQUIRE_FALSE(Socket::wrote("answered:x"));
        }
    }
}

SCENARIO("A deferred callback that fails rejects the call with its code", "[deferred]") {
    Browser browser;
    browser.app.cppFunction("failLater", &failLater);
    browser.connect();
    browser.call("failLater", 9, "x");
    gates.release(0);
    THEN("the code and the message are sent") {
        REQUIRE(Socket::wrote("quota:exceeded"));
        REQUIRE(Socket::wrote("the disk is full"));
    }
}

SCENARIO("A deferred callback that throws before returning its task rejects the call at once", "[deferred]") {
    Browser browser;
    browser.app.cppFunction<ResultTask<std::string>, std::string>("throwsNow", [](const std::string&) -> ResultTask<std::string> {
        throw BridgeError("early", "refused before it started");
    });
    browser.connect();
    browser.call("throwsNow", 4, "x");
    THEN("the rejection is sent without waiting") {
        REQUIRE(Socket::wrote("early"));
        REQUIRE(Socket::wrote("refused before it started"));
    }
}

SCENARIO("A deferred callback whose operation is stopped rejects the call as cancelled", "[deferred]") {
    Browser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 3, "x");
    gates.stopOne(0);
    THEN("the cancellation code is sent") {
        REQUIRE(Socket::wrote(std::string(bridge_code::cancelled)));
    }
}

SCENARIO("A callback may be a task without result", "[deferred]") {
    Browser browser;
    browser.app.cppFunction("voidLater", &voidLater);
    browser.connect();
    Socket::discardWrites();
    const auto before = Socket::discardedBytes();
    browser.call("voidLater", 2);
    REQUIRE(Socket::discardedBytes() == before);
    gates.release(0);
    THEN("a return without value is sent") {
        REQUIRE(Socket::discardedBytes() > before);
    }
}

SCENARIO("A callback taking the caller can answer later", "[deferred]") {
    Browser browser;
    browser.app.cppFunction<ResultTask<std::string>, App::UI, std::string>("withCaller", [](App::UI, const std::string& value) -> ResultTask<std::string> {
        return echoLater(value);
    });
    browser.connect();
    browser.call("withCaller", 6, "who");
    gates.release(0);
    THEN("the browser receives the value") {
        REQUIRE(Socket::wrote("answered:who"));
    }
}

SCENARIO("Several deferred calls of one connection are answered independently", "[deferred]") {
    Browser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 1, "first");
    browser.call("echoLater", 2, "second");
    browser.call("echoLater", 3, "third");
    REQUIRE(gates.open.size() == 3);
    gates.release(2);
    gates.release(0);
    THEN("each is answered when it completes, whatever the order") {
        REQUIRE(Socket::wrote("answered:third"));
        REQUIRE(Socket::wrote("answered:first"));
        REQUIRE_FALSE(Socket::wrote("answered:second"));
        gates.release(1);
        REQUIRE(Socket::wrote("answered:second"));
        REQUIRE(Socket::writtenAt("answered:third") < Socket::writtenAt("answered:first"));
    }
}

SCENARIO("A call identifier that is still in flight cannot be reused", "[deferred]") {
    Browser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 7, "first");
    browser.call("echoLater", 7, "again");
    THEN("the second call is rejected and the first one still completes") {
        REQUIRE(gates.open.size() == 1);
        REQUIRE(Socket::wrote("already in use"));
        gates.release(0);
        REQUIRE(Socket::wrote("answered:first"));
    }
    AND_WHEN("it was answered") {
        gates.release(0);
        browser.call("echoLater", 7, "later");
        THEN("the identifier is free again") {
            REQUIRE(gates.open.size() == 2);
        }
    }
}

SCENARIO("The disconnection of the browser stops its deferred calls and drops their answers", "[deferred]") {
    Browser browser;
    std::weak_ptr<int> observed;
    browser.app.cppFunction<ResultTask<std::string>>("watched", [&observed]() -> ResultTask<std::string> {
        return watchedLater(&observed, std::make_shared<int>(1));
    });
    browser.connect();
    browser.call("watched", 8);
    REQUIRE_FALSE(observed.expired());

    WHEN("the connection closes while the call is pending") {
        Socket::fail(std::make_error_code(std::errc::connection_reset));
        THEN("the work is stopped and destroyed, and no answer is sent") {
            REQUIRE(observed.expired());
            REQUIRE_FALSE(Socket::wrote("late"));
            REQUIRE_FALSE(Socket::wrote(std::string(bridge_code::cancelled)));
        }
    }
}

SCENARIO("Shutting the application down joins its pending deferred calls", "[deferred]") {
    Browser browser;
    std::weak_ptr<int> observed;
    browser.app.cppFunction<ResultTask<std::string>>("watched", [&observed]() -> ResultTask<std::string> {
        return watchedLater(&observed, std::make_shared<int>(1));
    });
    browser.connect();
    browser.call("watched", 8);
    REQUIRE_FALSE(observed.expired());
    browser.app.shutdown();
    THEN("the pending work is gone when shutdown returns") {
        REQUIRE(observed.expired());
        REQUIRE_FALSE(Socket::wrote("late"));
    }
}
