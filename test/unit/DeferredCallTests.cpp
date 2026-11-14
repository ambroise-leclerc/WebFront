// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief C++ callbacks that answer a JavaScript call later: their result, failures, stop, disconnection and shutdown (#459).
#include "InjectableNetworking.hpp"
#include "Bdd.hpp"
#include <webfront/WebFront.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace webfront::bdd;

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

Scenario callbackReturns{"A callback that returns a task answers when the task completes", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();

    when("the browser calls it", [&] {
        browser.call("echoLater", 5, "x");
        then("nothing is answered while the task is suspended", [&] {
            require(deferredGates.open.size() == 1);
            require(!Socket::wrote("answered:x"));
        });
        andWhen("the task completes", [&] {
            deferredGates.release(0);
            then("its value is sent as the result of the call", [&] {
                require(Socket::wrote("answered:x"));
            });
        });
    });
    when("the call asks for no answer", [&] {
        browser.call("echoLater", 0, "x");
        deferredGates.release(0);
        then("nothing is sent", [&] {
            require(!Socket::wrote("answered:x"));
        });
    });
}};

Scenario deferredCallback{"A deferred callback that fails rejects the call with its code", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("failLater", &failLater);
    browser.connect();
    browser.call("failLater", 9, "x");
    deferredGates.release(0);
    then("the code and the message are sent", [&] {
        require(Socket::wrote("quota:exceeded"));
        require(Socket::wrote("the disk is full"));
    });
}};

Scenario deferredCallbackThrows{"A deferred callback that throws before returning its task rejects the call at once", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction<ResultTask<std::string>, std::string>("throwsNow", [](const std::string&) -> ResultTask<std::string> {
        throw BridgeError("early", "refused before it started");
    });
    browser.connect();
    browser.call("throwsNow", 4, "x");
    then("the rejection is sent without waiting", [&] {
        require(Socket::wrote("early"));
        require(Socket::wrote("refused before it started"));
    });
}};

Scenario deferredCallbackWhose{"A deferred callback whose operation is stopped rejects the call as cancelled", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 3, "x");
    deferredGates.stopOne(0);
    then("the cancellation code is sent", [&] {
        require(Socket::wrote(std::string(bridge_code::cancelled)));
    });
}};

Scenario callbackMay{"A callback may be a task without result", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("voidLater", &voidLater);
    browser.connect();
    Socket::discardWrites();
    const auto before = Socket::discardedBytes();
    browser.call("voidLater", 2);
    require(Socket::discardedBytes() == before);
    deferredGates.release(0);
    then("a return without value is sent", [&] {
        require(Socket::discardedBytes() > before);
    });
}};

Scenario callbackTaking{"A callback taking the caller can answer later", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction<ResultTask<std::string>, DeferredApp::UI, std::string>("withCaller", [](DeferredApp::UI, const std::string& value) -> ResultTask<std::string> {
        return echoLater(value);
    });
    browser.connect();
    browser.call("withCaller", 6, "who");
    deferredGates.release(0);
    then("the browser receives the value", [&] {
        require(Socket::wrote("answered:who"));
    });
}};

Scenario severalDeferred{"Several deferred calls of one connection are answered independently", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.connect();
    browser.call("echoLater", 1, "first");
    browser.call("echoLater", 2, "second");
    browser.call("echoLater", 3, "third");
    require(deferredGates.open.size() == 3);
    deferredGates.release(2);
    deferredGates.release(0);
    then("each is answered when it completes, whatever the order", [&] {
        require(Socket::wrote("answered:third"));
        require(Socket::wrote("answered:first"));
        require(!Socket::wrote("answered:second"));
        deferredGates.release(1);
        require(Socket::wrote("answered:second"));
        require(Socket::writtenAt("answered:third") < Socket::writtenAt("answered:first"));
    });
}};

Scenario callReuses{"A call that reuses the identifier of a running call is dropped without any answer", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("echoLater", &echoLater);
    browser.app.cppFunction<std::string>("plain", [] { return std::string("plain-result"); });
    browser.connect();
    browser.call("echoLater", 7, "first");
    browser.call("echoLater", 7, "again");
    browser.call("plain", 7);
    browser.call("noSuchFunction", 7);
    then("no callback runs and nothing is sent for the duplicates, whatever the function", [&] {
        require(deferredGates.open.size() == 1);
        require(!Socket::wrote("plain-result"));
        require(!Socket::wrote("already in use"));
        require(!Socket::wrote("was not found"));
    });
    then("the first call still completes with its own answer", [&] {
        deferredGates.release(0);
        require(Socket::wrote("answered:first"));
        require(!Socket::wrote("answered:again"));
    });
    andWhen("it was answered", [&] {
        deferredGates.release(0);
        browser.call("echoLater", 7, "later");
        then("the identifier is free again", [&] {
            require(deferredGates.open.size() == 2);
        });
    });
}};

Scenario disconnectionBrowser{"The disconnection of the browser stops its deferred calls and drops their answers", {"deferred"}, [] {
    DeferredBrowser browser;
    std::weak_ptr<int> observed;
    browser.app.cppFunction<ResultTask<std::string>>("watched", [&observed]() -> ResultTask<std::string> {
        return watchedLater(&observed, std::make_shared<int>(1));
    });
    browser.connect();
    browser.call("watched", 8);
    require(!observed.expired());

    when("the connection closes while the call is pending", [&] {
        Socket::fail(std::make_error_code(std::errc::connection_reset));
        then("the work is stopped and destroyed, and no answer is sent", [&] {
            require(observed.expired());
            require(!Socket::wrote("late"));
            require(!Socket::wrote(std::string(bridge_code::cancelled)));
        });
    });
}};

Scenario shuttingApplication{"Shutting the application down joins its pending deferred calls", {"deferred"}, [] {
    DeferredBrowser browser;
    std::weak_ptr<int> observed;
    browser.app.cppFunction<ResultTask<std::string>>("watched", [&observed]() -> ResultTask<std::string> {
        return watchedLater(&observed, std::make_shared<int>(1));
    });
    browser.connect();
    browser.call("watched", 8);
    require(!observed.expired());
    browser.app.shutdown();
    then("the pending work is gone when shutdown returns", [&] {
        require(observed.expired());
        require(!Socket::wrote("late"));
    });
}};

Scenario identifiersFlight{"The identifiers of in-flight calls are claimed once and released on finish", {"deferred"}, [] {
    detail::DeferredCalls calls;
    const auto            key = calls.reserve(5);
    require(key != 0);
    require(calls.reserve(5) == 0);
    require(calls.reserve(0) != 0);  // zero asks for no answer: never in flight
    require(calls.reserve(0) != 0);
    when("the call finishes, as a task the server refused does at once", [&] {
        calls.finish(5, key);
        then("the identifier can be claimed again", [&] {
            require(calls.reserve(5) != 0);
        });
    });
    when("the connection closes", [&] {
        calls.close();
        then("nothing can be claimed any more", [&] {
            require(calls.reserve(6) == 0);
        });
    });
}};

Scenario decodedArguments{"The decoded arguments of a deferred callback live until it completes", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction("byReference", &byReferenceLater);
    browser.connect();
    browser.call("byReference", 3, std::string(60, 'r'));  // beyond the small string buffer: a dangling reference would read freed memory
    deferredGates.release(0);
    then("the coroutine still reads its argument", [&] {
        require(Socket::wrote("reference:" + std::string(60, 'r')));
    });
}};

Scenario callRefused{"A call refused as a duplicate does not run the callback", {"deferred"}, [] {
    DeferredBrowser browser;
    sideEffects = 0;
    browser.app.cppFunction<ResultTask<std::string>, std::string>("counting", [](const std::string& value) -> ResultTask<std::string> {
        ++sideEffects;  // an action taken before the task is returned
        return countingLater(value);
    });
    browser.connect();
    browser.call("counting", 7, "first");
    browser.call("counting", 7, "again");
    then("the callback ran once and the duplicate was refused", [&] {
        require(sideEffects == 1);
        require(!Socket::wrote("already in use"));
    });
}};

Scenario callingUI{"The calling UI of a deferred callback lives until it completes", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.app.cppFunction<ResultTask<std::string>, const DeferredApp::UI&, std::string>("uiReference", &uiReferenceLater);
    browser.app.cppFunction<ResultTask<std::string>, DeferredApp::UI&, std::string>("uiMutable", &uiMutableLater);
    browser.connect();
    browser.call("uiReference", 1, std::string(60, 'u'));
    browser.call("uiMutable", 2, std::string(60, 'm'));
    deferredGates.release(0);
    deferredGates.release(1);
    then("the coroutines still read the UI they were given", [&] {
        require(Socket::wrote(":" + std::string(60, 'u')));
        require(Socket::wrote(":" + std::string(60, 'm')));
        require(Socket::wrote("mutable"));
    });
}};

Scenario callUnknown{"A call to an unknown function is rejected with the not-found code of the bridge", {"deferred"}, [] {
    DeferredBrowser browser;
    browser.connect();
    browser.call("noSuchFunction", 4);
    then("the code reserved to the bridge reaches the browser", [&] {
        require(Socket::wrote(std::string(bridge_code::notFound)));
        require(Socket::wrote("was not found"));
    });
}};
