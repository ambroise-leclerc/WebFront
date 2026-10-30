#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <webfront/WebFront.hpp>

#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace webfront;
using namespace webfront::testing;
namespace ex = networking::ex;

namespace {
using CorrelationPolicy = http::CustomBuffersPolicy<64, 64, 64>;
struct SenderApp {
    using Net          = InjectableNetworking;
    using BufferPolicy = http::DefaultBuffersPolicy;
    WebLink<Net>& link;
    auto          findLink(WebLinkId) {
        return std::shared_ptr<WebLink<Net>>(std::shared_ptr<WebLink<Net>>{}, &link);  // not owned
    }
};
struct Observed {
    int                   completions{};
    bool                  stopped{};
    std::string           value;
    std::string           error;
    std::function<void()> onComplete;
    void                  done() noexcept {
        ++completions;
        if (onComplete)
            onComplete();
    }
};
struct Receiver {
    using receiver_concept = ex::receiver_tag;
    Observed&              observed;
    ex::inplace_stop_token token;
    auto                   get_env() const noexcept {
        return ex::prop{ex::get_stop_token, token};
    }
    void set_value(std::string value) && noexcept {
        observed.value = std::move(value);
        observed.done();
    }
    void set_value() && noexcept {
        observed.done();
    }
    void set_stopped() && noexcept {
        observed.stopped = true;
        observed.done();
    }
    void set_error(std::exception_ptr e) && noexcept {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& error) {
            observed.error = error.what();
        }
        observed.done();
    }
};
template <typename Configure>
void reply(msg::CallId id, Configure configure) {
    msg::FunctionReturn<> message;
    message.setCallId(id);
    websocket::Frame<InjectableNetworking> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    configure(message, frame);
    InjectableSocket::receive(clientFrame(messagePayload(frame)));
}
void stringReply(msg::CallId id, std::string value) {
    reply(id, [&](auto& message, auto& frame) {
        message.encodeParameter(value, frame);
        frame.freeze();
    });
}
}  // namespace

SCENARIO("Typed browser senders are lazy and own their arguments") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) { }, scope};
    SenderApp                     app{link};
    BasicUI<SenderApp>            ui{app, 1};
    ex::inplace_stop_source       stop;
    Observed                      observed;
    GIVEN("a sender encoded from a temporary string view") {
        auto sender = ui.jsFunctionSender<std::string>("echo")(std::string_view(std::string("owned-argument")));
        REQUIRE_FALSE(InjectableSocket::hasWrittenData());
        auto operation = ex::connect(std::move(sender), Receiver{observed, stop.get_token()});
        static_assert(!std::is_copy_constructible_v<decltype(operation)>);
        static_assert(!std::is_move_constructible_v<decltype(operation)>);
        REQUIRE_FALSE(InjectableSocket::hasWrittenData());
        WHEN("the operation starts") {
            ex::start(operation);
            REQUIRE(InjectableSocket::wrote("owned-argument"));
            REQUIRE(observed.completions == 0);
            stringReply(1, "result");
            THEN("the receiver gets the decoded result once") {
                CHECK(observed.value == "result");
                CHECK(observed.completions == 1);
                stop.request_stop();
                CHECK_FALSE(observed.stopped);
            }
        }
    }
    GIVEN("a void sender") {
        auto operation = ex::connect(ui.jsFunctionSender("finish")(), Receiver{observed, stop.get_token()});
        ex::start(operation);
        reply(1, [](auto&, auto&) { });
        THEN("an empty return completes successfully") {
            CHECK(observed.completions == 1);
            CHECK(observed.error.empty());
        }
    }
}

SCENARIO("Typed browser senders report errors without escaping the receive loop") {
    InjectableSocket::reset();
    InlineTaskScope         scope;
    auto                    link = std::make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{1}, [](WebLinkEvent) { }, scope);
    SenderApp               app{*link};
    BasicUI<SenderApp>      ui{app, 1};
    Observed                observed;
    ex::inplace_stop_source stop;
    auto                    sender = ui.jsFunctionSender<std::string>("result")();
    GIVEN("a call started before the browser responds") {
        auto operation = ex::connect(sender, Receiver{observed, stop.get_token()});
        ex::start(operation);
        WHEN("JavaScript throws") {
            reply(1, [](auto& message, auto& frame) {
                const std::runtime_error error("JS failed");
                message.encodeParameter(error, frame);
                frame.freeze();
            });
            THEN("set_error carries its exception") {
                CHECK(observed.error == "JS failed");
            }
        }
        WHEN("the result is malformed") {
            reply(1, [](auto& message, auto&) {
                message.setParametersCount(1);
            });
            THEN("set_error carries the decoding error") {
                CHECK_FALSE(observed.error.empty());
            }
        }
        WHEN("the connection closes") {
            InjectableSocket::fail(std::make_error_code(std::errc::connection_reset));
            THEN("set_error reports disconnection") {
                CHECK(observed.error.starts_with("Browser connection closed"));
            }
        }
        WHEN("the link is destroyed") {
            link.reset();
            THEN("the receiver is completed before destruction returns") {
                CHECK(observed.error == "Browser connection closed");
            }
        }
        CHECK(observed.completions == 1);
    }
    GIVEN("a sender whose link has already been destroyed") {
        link.reset();
        auto operation = ex::connect(std::move(sender), Receiver{observed, stop.get_token()});
        ex::start(operation);
        THEN("starting it safely reports disconnection") {
            CHECK(observed.error == "Browser connection closed");
            CHECK(observed.completions == 1);
        }
    }
}

SCENARIO("Typed browser calls propagate receiver cancellation") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) { }, scope};
    SenderApp                     app{link};
    BasicUI<SenderApp>            ui{app, 1};
    ex::inplace_stop_source       stop;
    Observed                      observed;
    GIVEN("a receiver already stopped at start") {
        stop.request_stop();
        auto operation = ex::connect(ui.jsFunctionSender<std::string>("slow")(), Receiver{observed, stop.get_token()});
        ex::start(operation);
        THEN("it completes stopped without dispatching") {
            CHECK(observed.stopped);
            CHECK(observed.completions == 1);
            CHECK_FALSE(InjectableSocket::hasWrittenData());
        }
    }
    GIVEN("a call awaiting its browser result") {
        auto operation = ex::connect(ui.jsFunctionSender<std::string>("slow")(), Receiver{observed, stop.get_token()});
        ex::start(operation);
        stop.request_stop();
        THEN("the receiver completes stopped exactly once") {
            REQUIRE(observed.stopped);
            REQUIRE(observed.completions == 1);
        }
        WHEN("the browser returns late and a different call is pending") {
            auto [id, future] = link.expectResult<std::string>();
            REQUIRE(id != 1);
            stringReply(1, "late");
            CHECK(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
            stringReply(id, "next");
            THEN("only the matching call completes") {
                CHECK(future.get() == "next");
                CHECK(observed.completions == 1);
            }
        }
    }
    GIVEN("an operation destroyed by its own completion") {
        auto sender         = ui.jsFunctionSender<std::string>("slow")();
        using Operation     = ex::connect_result_t<decltype(sender), Receiver>;
        auto operation      = std::unique_ptr<Operation>(new Operation(ex::connect(std::move(sender), Receiver{observed, stop.get_token()})));
        observed.onComplete = [&] {
            operation.reset();
        };
        ex::start(*operation);
        stop.request_stop();
        THEN("cancellation safely releases the operation inline") {
            CHECK_FALSE(operation);
            CHECK(observed.stopped);
            CHECK(observed.completions == 1);
        }
    }
}

SCENARIO("Browser result and cancellation can race") {
    GIVEN("a dispatched call with a stoppable receiver") {
        WHEN("stop and a result arrive on different threads") {
            THEN("exactly one terminal completion wins on repeated attempts") {
                for (int iteration = 0; iteration != 40; ++iteration) {
                    InjectableSocket::reset();
                    InlineTaskScope               scope;
                    WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) { }, scope};
                    SenderApp                     app{link};
                    BasicUI<SenderApp>            ui{app, 1};
                    ex::inplace_stop_source       stop;
                    Observed                      observed;
                    auto                          operation = ex::connect(ui.jsFunctionSender<std::string>("race")(), Receiver{observed, stop.get_token()});
                    ex::start(operation);
                    std::thread stopping([&] {
                        stop.request_stop();
                    });
                    stringReply(1, "returned");
                    stopping.join();
                    CHECK(observed.completions == 1);
                    CHECK((observed.stopped || observed.value == "returned"));
                }
            }
        }
    }
}

namespace {
networking::Task awaitBrowser(BasicUI<SenderApp> ui, Observed& observed, std::shared_ptr<int> lifetime) {
    static_cast<void>(lifetime);
    observed.value = co_await ui.jsFunctionSender<std::string>("awaited")();
    observed.done();
}
}  // namespace
SCENARIO("Typed browser senders can be awaited in a tracked task") {
    InjectableSocket::reset();
    InlineTaskScope               transport;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) { }, transport};
    SenderApp                     app{link};
    Observed                      observed;
    InlineTaskScope               calls;
    GIVEN("a coroutine awaiting a browser sender in its own scope") {
        auto               lifetime = std::make_shared<int>(42);
        std::weak_ptr<int> frame    = lifetime;
        networking::Spawn(calls, ex::inline_scheduler{}, awaitBrowser(BasicUI<SenderApp>{app, 1}, observed, std::move(lifetime)));
        REQUIRE_FALSE(frame.expired());
        WHEN("the browser replies") {
            stringReply(1, "awaited result");
            THEN("co_await resumes with its typed value") {
                CHECK(observed.value == "awaited result");
                CHECK(observed.completions == 1);
                CHECK(frame.expired());
            }
        }
        WHEN("the enclosing scope requests stop while the transport remains active") {
            calls.requestStop();
            THEN("the pending coroutine unwinds before a late reply arrives") {
                CHECK(frame.expired());
                stringReply(1, "late");
                CHECK(observed.completions == 0);
                CHECK(observed.value.empty());
            }
        }
    }
}

SCENARIO("Cancelled correlation IDs cannot be reused before a late reply") {
    GIVEN("a registry with one cancelled call") {
        webfront::detail::PendingCalls<CorrelationPolicy> calls;
        auto                                    retained  = std::make_shared<int>(42);
        std::weak_ptr<int>                      released  = retained;
        const auto                              cancelled = calls.add({[retained](const auto&) { }, [retained](auto) { }});
        retained.reset();
        calls.cancel(cancelled);
        CHECK(released.expired());
        WHEN("the correlation counter wraps") {
            for (std::size_t i = 1; i < std::numeric_limits<msg::CallId>::max(); ++i) {
                const auto id = calls.add({[](const auto&) { }, [](auto) { }});
                REQUIRE(id != cancelled);
                msg::FunctionReturn<CorrelationPolicy> result;
                result.setCallId(id);
                REQUIRE(calls.complete(result));
            }
            const auto next = calls.add({[](const auto&) { }, [](auto) { }});
            THEN("the reserved ID is skipped until its response is consumed") {
                CHECK(next != cancelled);
                msg::FunctionReturn<CorrelationPolicy> late;
                late.setCallId(cancelled);
                CHECK(calls.complete(late));
            }
        }
    }
}

SCENARIO("A server scope can release the final owner of a WebSocket during cancellation") {
    InjectableSocket::reset();
    InlineTaskScope scope;
    GIVEN("a WebSocket owned only by its pending read and write tasks") {
        auto                                                      socket   = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
        std::weak_ptr<websocket::WebSocket<InjectableNetworking>> lifetime = socket;
        socket->start();
        InjectableSocket::deferWrites();
        socket->write(std::string_view("pending"));
        socket.reset();
        REQUIRE_FALSE(lifetime.expired());
        WHEN("the parent scope requests stop inline") {
            scope.requestStop();
            THEN("both tasks release the socket before the stop callback returns") {
                CHECK(lifetime.expired());
                CHECK(InjectableSocket::completeDeferredWrites() == 1);
            }
        }
    }
}

SCENARIO("Browser senders reject inconsistent return envelopes") {
    InjectableSocket::reset();
    InlineTaskScope               scope;
    WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) { }, scope};
    SenderApp                     app{link};
    BasicUI<SenderApp>            ui{app, 1};
    ex::inplace_stop_source       stop;
    Observed                      observed;
    GIVEN("a pending string call") {
        auto operation = ex::connect(ui.jsFunctionSender<std::string>("result")(), Receiver{observed, stop.get_token()});
        ex::start(operation);
        WHEN("a return contains no result") {
            reply(1, [](auto&, auto&) { });
            THEN("its arity error reaches the receiver") {
                CHECK(observed.error == "Malformed function return message");
            }
        }
        WHEN("a return contains trailing value data") {
            reply(1, [](auto& message, auto& frame) {
                message.encodeParameter("first", frame);
                message.encodeParameter("extra", frame);
                message.setParametersCount(1);
                frame.freeze();
            });
            THEN("the extra bytes are rejected") {
                CHECK(observed.error == "Function return contains trailing data");
            }
        }
        WHEN("an encoded exception contains trailing data") {
            reply(1, [](auto& message, auto& frame) {
                const std::runtime_error error("failure");
                message.encodeParameter(error, frame);
                message.encodeParameter("extra", frame);
                message.setParametersCount(1);
                frame.freeze();
            });
            THEN("the malformed exception is rejected") {
                CHECK(observed.error == "Malformed exception return payload");
            }
        }
        CHECK(observed.completions == 1);
    }
    GIVEN("a pending void call") {
        auto operation = ex::connect(ui.jsFunctionSender("finish")(), Receiver{observed, stop.get_token()});
        ex::start(operation);
        reply(1, [](auto& message, auto& frame) {
            message.encodeParameter("unexpected", frame);
            frame.freeze();
        });
        THEN("a nonempty result is rejected") {
            CHECK(observed.error == "Malformed void return message");
            CHECK(observed.completions == 1);
        }
    }
    GIVEN("a browser reply whose correlation ID was never registered") {
        stringReply(99, "unknown");
        THEN("the receive loop still accepts the next valid call") {
            auto [id, future] = link.expectResult<std::string>();
            stringReply(id, "valid");
            CHECK(future.get() == "valid");
        }
    }
}

SCENARIO("Correlation exhaustion fails until an outstanding reply releases an ID") {
    GIVEN("a registry with every ID reserved by an unanswered cancelled call") {
        webfront::detail::PendingCalls<CorrelationPolicy> calls;
        for (std::size_t i = 0; i < std::numeric_limits<msg::CallId>::max(); ++i) {
            const auto id = calls.add({});
            calls.cancel(id);
        }
        WHEN("another call is registered") {
            THEN("registration fails instead of reusing an unanswered ID") {
                CHECK_THROWS_WITH(calls.add({}), "Too many outstanding WebFront calls");
            }
        }
        WHEN("the browser returns one of the cancelled calls") {
            msg::FunctionReturn<CorrelationPolicy> result;
            result.setCallId(7);
            REQUIRE(calls.complete(result));
            THEN("its ID is available again") {
                CHECK(calls.add({}) == 7);
            }
        }
    }
}

SCENARIO("A sender reports a socket expiring independently of its correlation registry") {
    GIVEN("an expired socket and a still-owned registry") {
        auto                   calls = std::make_shared<webfront::detail::PendingCalls<http::DefaultBuffersPolicy>>();
        std::vector<std::byte> payload(sizeof(msg::FunctionCallHeader<msg::Command::callFunction>));
        webfront::detail::BrowserCallSender<InjectableNetworking, http::DefaultBuffersPolicy, std::string> sender{calls, {}, std::move(payload)};
        Observed                                                                                 observed;
        ex::inplace_stop_source                                                                  stop;
        auto operation = ex::connect(std::move(sender), Receiver{observed, stop.get_token()});
        WHEN("the operation starts") {
            ex::start(operation);
            THEN("the receiver gets a connection error without accessing the socket") {
                CHECK(observed.error == "Browser connection closed");
                CHECK(observed.completions == 1);
            }
        }
    }
}

SCENARIO("An obsolete registration cannot cancel a replacement with the same wire ID") {
    GIVEN("a completed call whose operation is still delivering an error") {
        webfront::detail::PendingCalls<CorrelationPolicy> calls;
        int                                     originalOwner{};
        int                                     replacementOwner{};
        const auto                              old = calls.add({.complete = [](const auto&) { }, .reject = {}, .owner = &originalOwner});
        msg::FunctionReturn<CorrelationPolicy>  result;
        result.setCallId(old);
        REQUIRE(calls.complete(result));
        for (std::size_t i = 1; i < std::numeric_limits<msg::CallId>::max(); ++i) {
            result.setCallId(calls.add({}));
            REQUIRE(calls.complete(result));
        }
        bool       completed   = false;
        const auto replacement = calls.add({.complete =
                                                [&](const auto&) {
                                                    completed = true;
                                                },
                                            .reject = {},
                                            .owner  = &replacementOwner});
        REQUIRE(replacement == old);
        WHEN("the obsolete operation tries to discard its registration") {
            calls.cancel(old, &originalOwner);
            result.setCallId(replacement);
            REQUIRE(calls.complete(result));
            THEN("the replacement still receives its own reply") {
                CHECK(completed);
            }
        }
    }
}
