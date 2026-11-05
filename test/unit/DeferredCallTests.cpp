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

const std::string deferredUpgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

struct DeferredNet : testing::InjectableNetworking {
    static inline bool accepted{};
    static auto        Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (std::exchange(accepted, true))
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

using DeferredApp = BasicWF<DeferredNet, fs::DefaultFS>;

/// The operations the test completes by hand, by the name of the call that awaits them.
struct DeferredGates {
    std::vector<testing::ManualSender::Completion> open;
    void                                           release(std::size_t index) { open.at(index).complete({}, 0); }
    void                                           stopOne(std::size_t index) { open.at(index).stop(); }
};
DeferredGates deferredGates;

testing::ManualSender deferredGate() {
    return testing::ManualSender([](testing::ManualSender::Completion completion) { deferredGates.open.push_back(std::move(completion)); });
}

ResultTask<std::string> echoLater(std::string value) {
    co_await deferredGate();
    co_return "answered:" + value;
}

/// Takes its argument by reference and uses it after suspending: the decoded argument must outlive the handler.
ResultTask<std::string> byReferenceLater(const std::string& value) {
    co_await deferredGate();
    co_return "reference:" + value;
}

/// Takes the calling UI by reference and reads it after suspending.
ResultTask<std::string> uiReferenceLater(const DeferredApp::UI& caller, const std::string& value) {
    co_await deferredGate();
    co_return "ui" + std::to_string(caller.id()) + ":" + value;
}

ResultTask<std::string> uiMutableLater(DeferredApp::UI& caller, std::string value) {
    co_await deferredGate();
    co_return "mutable" + std::to_string(caller.id()) + ":" + value;
}

int sideEffects = 0;

ResultTask<std::string> countingLater(std::string value) {
    co_await deferredGate();
    co_return "counted:" + value;
}

ResultTask<std::string> failLater(std::string value) {
    co_await deferredGate();
    if (!value.empty())  // always: keeps a value-returning coroutine free of unreachable code or a missing co_return
        throw BridgeError("quota:exceeded", "the disk is full");
    co_return value;
}

ResultTask<void> voidLater() {
    co_await deferredGate();
}

ResultTask<std::string> watchedLater(std::weak_ptr<int>* observed, std::shared_ptr<int> lifetime) {
    *observed = lifetime;
    co_await deferredGate();
    co_return "late";
}

/// A call from the browser: the function, its identifier and one string argument.
std::vector<std::byte> deferredCallMessage(const std::string& name, msg::CallId callId, const std::string& argument = "") {
    msg::FunctionCall<> message;
    message.setCallId(callId);
    websocket::Frame<DeferredNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(name, frame);
    if (!argument.empty())
        message.encodeParameter(argument, frame);
    frame.freeze();
    return testing::clientFrame(testing::messagePayload(frame));
}

/// Resets the shared mock socket before the application accepts its connection.
struct DeferredFresh {
    DeferredFresh() {
        Socket::reset();
        deferredGates.open.clear();
        DeferredNet::accepted = false;
    }
};

struct DeferredBrowser {
    DeferredFresh fresh;
    DeferredApp   app{WebFrontConfig{.port = "0"}};

    void connect() {
        Socket::receive(std::as_bytes(std::span(deferredUpgradeRequest)));
        msg::Handshake handshake;
        Socket::receive(testing::clientFrame(std::span(reinterpret_cast<const std::byte*>(handshake.header().data()), handshake.header().size())));
    }
    void call(const std::string& name, msg::CallId callId, const std::string& argument = "") {
        Socket::receive(deferredCallMessage(name, callId, argument));
    }
    ~DeferredBrowser() {
        app.shutdown();
        Socket::reset();
        deferredGates.open.clear();
    }
};

}  // namespace

SCENARIO("A callback that returns a task answers when the task completes", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();

    WHEN("the browser calls it") {
        browser.call("echoLater", 5, "x");
        THEN("nothing is answered while the task is suspended") {
            REQUIRE(deferredGates.open.size() == 1);
            REQUIRE_FALSE(Socket::wrote("answered:x"));
        }
        AND_WHEN("the task completes") {
            deferredGates.release(0);
            THEN("its value is sent as the result of the call") {
                REQUIRE(Socket::wrote("answered:x"));
            }
        }
    }
    WHEN("the call asks for no answer") {
        browser.call("echoLater", 0, "x");
        deferredGates.release(0);
        THEN("nothing is sent") {
            REQUIRE_FALSE(Socket::wrote("answered:x"));
        }
    }
}

SCENARIO("A deferred callback that fails rejects the call with its code", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction("failLater", &failLater);
    browser.connect();
    browser.call("failLater", 9, "x");
    deferredGates.release(0);
    THEN("the code and the message are sent") {
        REQUIRE(Socket::wrote("quota:exceeded"));
        REQUIRE(Socket::wrote("the disk is full"));
    }
}

SCENARIO("A deferred callback that throws before returning its task rejects the call at once", "[deferred]") {
    DeferredBrowser browser;
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
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 3, "x");
    deferredGates.stopOne(0);
    THEN("the cancellation code is sent") {
        REQUIRE(Socket::wrote(std::string(bridge_code::cancelled)));
    }
}

SCENARIO("A callback may be a task without result", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction("voidLater", &voidLater);
    browser.connect();
    Socket::discardWrites();
    const auto before = Socket::discardedBytes();
    browser.call("voidLater", 2);
    REQUIRE(Socket::discardedBytes() == before);
    deferredGates.release(0);
    THEN("a return without value is sent") {
        REQUIRE(Socket::discardedBytes() > before);
    }
}

SCENARIO("A callback taking the caller can answer later", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction<ResultTask<std::string>, DeferredApp::UI, std::string>("withCaller", [](DeferredApp::UI, const std::string& value) -> ResultTask<std::string> {
        return echoLater(value);
    });
    browser.connect();
    browser.call("withCaller", 6, "who");
    deferredGates.release(0);
    THEN("the browser receives the value") {
        REQUIRE(Socket::wrote("answered:who"));
    }
}

SCENARIO("Several deferred calls of one connection are answered independently", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 1, "first");
    browser.call("echoLater", 2, "second");
    browser.call("echoLater", 3, "third");
    REQUIRE(deferredGates.open.size() == 3);
    deferredGates.release(2);
    deferredGates.release(0);
    THEN("each is answered when it completes, whatever the order") {
        REQUIRE(Socket::wrote("answered:third"));
        REQUIRE(Socket::wrote("answered:first"));
        REQUIRE_FALSE(Socket::wrote("answered:second"));
        deferredGates.release(1);
        REQUIRE(Socket::wrote("answered:second"));
        REQUIRE(Socket::writtenAt("answered:third") < Socket::writtenAt("answered:first"));
    }
}

SCENARIO("A call that reuses the identifier of a running call is dropped without any answer", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.app.cppFunction<std::string>("plain", [] { return std::string("plain-result"); });
    browser.connect();
    browser.call("echoLater", 7, "first");
    browser.call("echoLater", 7, "again");
    browser.call("plain", 7);
    browser.call("noSuchFunction", 7);
    THEN("no callback runs and nothing is sent for the duplicates, whatever the function") {
        REQUIRE(deferredGates.open.size() == 1);
        REQUIRE_FALSE(Socket::wrote("plain-result"));
        REQUIRE_FALSE(Socket::wrote("already in use"));
        REQUIRE_FALSE(Socket::wrote("was not found"));
    }
    THEN("the first call still completes with its own answer") {
        deferredGates.release(0);
        REQUIRE(Socket::wrote("answered:first"));
        REQUIRE_FALSE(Socket::wrote("answered:again"));
    }
    AND_WHEN("it was answered") {
        deferredGates.release(0);
        browser.call("echoLater", 7, "later");
        THEN("the identifier is free again") {
            REQUIRE(deferredGates.open.size() == 2);
        }
    }
}

SCENARIO("The disconnection of the browser stops its deferred calls and drops their answers", "[deferred]") {
    DeferredBrowser browser;
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
    DeferredBrowser browser;
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

SCENARIO("The identifiers of in-flight calls are claimed once and released on finish", "[deferred]") {
    detail::DeferredCalls calls;
    const auto            key = calls.reserve(5);
    REQUIRE(key != 0);
    REQUIRE(calls.reserve(5) == 0);
    REQUIRE(calls.reserve(0) != 0);  // zero asks for no answer: never in flight
    REQUIRE(calls.reserve(0) != 0);
    WHEN("the call finishes, as a task the server refused does at once") {
        calls.finish(5, key);
        THEN("the identifier can be claimed again") {
            REQUIRE(calls.reserve(5) != 0);
        }
    }
    WHEN("the connection closes") {
        calls.close();
        THEN("nothing can be claimed any more") {
            REQUIRE(calls.reserve(6) == 0);
        }
    }
}

SCENARIO("The decoded arguments of a deferred callback live until it completes", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction("byReference", &byReferenceLater);
    browser.connect();
    browser.call("byReference", 3, std::string(60, 'r'));  // beyond the small string buffer: a dangling reference would read freed memory
    deferredGates.release(0);
    THEN("the coroutine still reads its argument") {
        REQUIRE(Socket::wrote("reference:" + std::string(60, 'r')));
    }
}

SCENARIO("A call refused as a duplicate does not run the callback", "[deferred]") {
    DeferredBrowser browser;
    sideEffects = 0;
    browser.app.cppFunction<ResultTask<std::string>, std::string>("counting", [](const std::string& value) -> ResultTask<std::string> {
        ++sideEffects;  // an action taken before the task is returned
        return countingLater(value);
    });
    browser.connect();
    browser.call("counting", 7, "first");
    browser.call("counting", 7, "again");
    THEN("the callback ran once and the duplicate was refused") {
        REQUIRE(sideEffects == 1);
        REQUIRE_FALSE(Socket::wrote("already in use"));
    }
}

SCENARIO("The calling UI of a deferred callback lives until it completes", "[deferred]") {
    DeferredBrowser browser;
    browser.app.cppFunction<ResultTask<std::string>, const DeferredApp::UI&, std::string>("uiReference", &uiReferenceLater);
    browser.app.cppFunction<ResultTask<std::string>, DeferredApp::UI&, std::string>("uiMutable", &uiMutableLater);
    browser.connect();
    browser.call("uiReference", 1, std::string(60, 'u'));
    browser.call("uiMutable", 2, std::string(60, 'm'));
    deferredGates.release(0);
    deferredGates.release(1);
    THEN("the coroutines still read the UI they were given") {
        REQUIRE(Socket::wrote(":" + std::string(60, 'u')));
        REQUIRE(Socket::wrote(":" + std::string(60, 'm')));
        REQUIRE(Socket::wrote("mutable"));
    }
}

SCENARIO("A call to an unknown function is rejected with the not-found code of the bridge", "[deferred]") {
    DeferredBrowser browser;
    browser.connect();
    browser.call("noSuchFunction", 4);
    THEN("the code reserved to the bridge reaches the browser") {
        REQUIRE(Socket::wrote(std::string(bridge_code::notFound)));
        REQUIRE(Socket::wrote("was not found"));
    }
}
