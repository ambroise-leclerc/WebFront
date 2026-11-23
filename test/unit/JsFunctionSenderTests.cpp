#include "InjectableNetworking.hpp"
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace webfront::bdd;

using namespace webfront;
using namespace webfront::testing;
namespace ex = networking::ex;

namespace {
using CorrelationPolicy = http::CustomBuffersPolicy<64, 64, 64>;
struct SenderApp {
    using Net          = InjectableNetworking;
    using BufferPolicy = http::DefaultBuffersPolicy;
    WebLink<Net>& link;
    auto          findLink(WebLinkId, WebLinkGeneration) {
        return std::shared_ptr<WebLink<Net>>(std::shared_ptr<WebLink<Net>>{}, &link);  // not owned
    }
};
struct SenderObserved {
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
    SenderObserved&        observed;
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

const Scenario typedBrowser{"Typed browser senders are lazy and own their arguments", [] {
                                InjectableSocket::reset();
                                InlineTaskScope               scope;
                                WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) {}, scope};
                                SenderApp                     app{link};
                                BasicUI<SenderApp>            ui{app, 1, 1};
                                ex::inplace_stop_source       stop;
                                SenderObserved                observed;
                                given("a sender encoded from a temporary string view", [&] {
                                    auto sender = ui.jsFunctionSender<std::string>("echo")(std::string_view(std::string("owned-argument")));
                                    require(!InjectableSocket::hasWrittenData());
                                    auto operation = ex::connect(std::move(sender), Receiver{observed, stop.get_token()});
                                    static_assert(!std::is_copy_constructible_v<decltype(operation)>);
                                    static_assert(!std::is_move_constructible_v<decltype(operation)>);
                                    require(!InjectableSocket::hasWrittenData());
                                    when("the operation starts", [&] {
                                        ex::start(operation);
                                        require(InjectableSocket::wrote("owned-argument"));
                                        require(observed.completions == 0);
                                        stringReply(1, "result");
                                        then("the receiver gets the decoded result once", [&] {
                                            check(observed.value == "result");
                                            check(observed.completions == 1);
                                            stop.request_stop();
                                            check(!observed.stopped);
                                        });
                                    });
                                });
                                given("a void sender", [&] {
                                    auto operation = ex::connect(ui.jsFunctionSender("finish")(), Receiver{observed, stop.get_token()});
                                    ex::start(operation);
                                    reply(1, [](auto&, auto&) {});
                                    then("an empty return completes successfully", [&] {
                                        check(observed.completions == 1);
                                        check(observed.error.empty());
                                    });
                                });
                            }};

const Scenario typedBrowserSenders{
    "Typed browser senders report errors without escaping the receive loop",
    [] {
        InjectableSocket::reset();
        InlineTaskScope         scope;
        auto                    link = std::make_unique<WebLink<InjectableNetworking>>(InjectableSocket{}, WebLinkId{1}, [](WebLinkEvent) {}, scope);
        SenderApp               app{*link};
        BasicUI<SenderApp>      ui{app, 1, 1};
        SenderObserved          observed;
        ex::inplace_stop_source stop;
        auto                    sender = ui.jsFunctionSender<std::string>("result")();
        given("a call started before the browser responds", [&] {
            auto operation = ex::connect(sender, Receiver{observed, stop.get_token()});
            ex::start(operation);
            when("JavaScript throws", [&] {
                reply(1, [](auto& message, auto& frame) {
                    const std::runtime_error error("JS failed");
                    message.encodeParameter(error, frame);
                    frame.freeze();
                });
                then("set_error carries its exception", [&] {
                    check(observed.error == "JS failed");
                });
            });
            when("the result is malformed", [&] {
                reply(1, [](auto& message, auto&) {
                    message.setParametersCount(1);
                });
                then("set_error carries the decoding error", [&] {
                    check(!observed.error.empty());
                });
            });
            when("the connection closes", [&] {
                InjectableSocket::fail(std::make_error_code(std::errc::connection_reset));
                then("set_error reports disconnection", [&] {
                    check(observed.error.starts_with("Browser connection closed"));
                });
            });
            when("the link is destroyed", [&] {
                link.reset();
                then("the receiver is completed before destruction returns", [&] {
                    check(observed.error == "Browser connection closed");
                });
            });
            check(observed.completions == 1);
        });
        given("a sender whose link has already been destroyed", [&] {
            link.reset();
            auto operation = ex::connect(std::move(sender), Receiver{observed, stop.get_token()});
            ex::start(operation);
            then("starting it safely reports disconnection", [&] {
                check(observed.error == "Browser connection closed");
                check(observed.completions == 1);
            });
        });
    }};

const Scenario typedBrowserCalls{"Typed browser calls propagate receiver cancellation", [] {
                                     InjectableSocket::reset();
                                     InlineTaskScope               scope;
                                     WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) {}, scope};
                                     SenderApp                     app{link};
                                     BasicUI<SenderApp>            ui{app, 1, 1};
                                     ex::inplace_stop_source       stop;
                                     SenderObserved                observed;
                                     given("a receiver already stopped at start", [&] {
                                         stop.request_stop();
                                         auto operation = ex::connect(ui.jsFunctionSender<std::string>("slow")(), Receiver{observed, stop.get_token()});
                                         ex::start(operation);
                                         then("it completes stopped without dispatching", [&] {
                                             check(observed.stopped);
                                             check(observed.completions == 1);
                                             check(!InjectableSocket::hasWrittenData());
                                         });
                                     });
                                     given("a call awaiting its browser result", [&] {
                                         auto operation = ex::connect(ui.jsFunctionSender<std::string>("slow")(), Receiver{observed, stop.get_token()});
                                         ex::start(operation);
                                         stop.request_stop();
                                         then("the receiver completes stopped exactly once", [&] {
                                             require(observed.stopped);
                                             require(observed.completions == 1);
                                         });
                                         when("the browser returns late and a different call is pending", [&] {
                                             auto [id, future] = link.expectResult<std::string>();
                                             require(id != 1);
                                             stringReply(1, "late");
                                             check(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
                                             stringReply(id, "next");
                                             then("only the matching call completes", [&] {
                                                 check(future.get() == "next");
                                                 check(observed.completions == 1);
                                             });
                                         });
                                     });
                                     given("an operation destroyed by its own completion", [&] {
                                         auto sender     = ui.jsFunctionSender<std::string>("slow")();
                                         using Operation = ex::connect_result_t<decltype(sender), Receiver>;
                                         auto operation  = std::unique_ptr<Operation>(
                                             new Operation(ex::connect(std::move(sender), Receiver{observed, stop.get_token()})));
                                         observed.onComplete = [&] {
                                             operation.reset();
                                         };
                                         ex::start(*operation);
                                         stop.request_stop();
                                         then("cancellation safely releases the operation inline", [&] {
                                             check(!operation);
                                             check(observed.stopped);
                                             check(observed.completions == 1);
                                         });
                                     });
                                 }};

const Scenario browserResult{"Browser result and cancellation can race", [] {
                                 given("a dispatched call with a stoppable receiver", [&] {
                                     when("stop and a result arrive on different threads", [&] {
                                         then("exactly one terminal completion wins on repeated attempts", [&] {
                                             for (int iteration = 0; iteration != 40; ++iteration) {
                                                 InjectableSocket::reset();
                                                 InlineTaskScope               scope;
                                                 WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) {}, scope};
                                                 SenderApp                     app{link};
                                                 BasicUI<SenderApp>            ui{app, 1, 1};
                                                 ex::inplace_stop_source       stop;
                                                 SenderObserved                observed;
                                                 auto operation = ex::connect(ui.jsFunctionSender<std::string>("race")(), Receiver{observed, stop.get_token()});
                                                 ex::start(operation);
                                                 std::thread stopping([&] {
                                                     stop.request_stop();
                                                 });
                                                 stringReply(1, "returned");
                                                 stopping.join();
                                                 check(observed.completions == 1);
                                                 check((observed.stopped || observed.value == "returned"));
                                             }
                                         });
                                     });
                                 });
                             }};

namespace {
networking::Task awaitBrowser(BasicUI<SenderApp> ui, SenderObserved& observed, std::shared_ptr<int> lifetime) {
    static_cast<void>(lifetime);
    observed.value = co_await ui.jsFunctionSender<std::string>("awaited")();
    observed.done();
}
}  // namespace
const Scenario typedBrowserSendersCan{"Typed browser senders can be awaited in a tracked task", [] {
                                          InjectableSocket::reset();
                                          InlineTaskScope               transport;
                                          WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) {}, transport};
                                          SenderApp                     app{link};
                                          SenderObserved                observed;
                                          InlineTaskScope               calls;
                                          given("a coroutine awaiting a browser sender in its own scope", [&] {
                                              auto               lifetime = std::make_shared<int>(42);
                                              std::weak_ptr<int> frame    = lifetime;
                                              networking::Spawn(calls,
                                                                ex::inline_scheduler{},
                                                                awaitBrowser(BasicUI<SenderApp>{app, 1, 1}, observed, std::move(lifetime)));
                                              require(!frame.expired());
                                              when("the browser replies", [&] {
                                                  stringReply(1, "awaited result");
                                                  then("co_await resumes with its typed value", [&] {
                                                      check(observed.value == "awaited result");
                                                      check(observed.completions == 1);
                                                      check(frame.expired());
                                                  });
                                              });
                                              when("the enclosing scope requests stop while the transport remains active", [&] {
                                                  calls.requestStop();
                                                  then("the pending coroutine unwinds before a late reply arrives", [&] {
                                                      check(frame.expired());
                                                      stringReply(1, "late");
                                                      check(observed.completions == 0);
                                                      check(observed.value.empty());
                                                  });
                                              });
                                          });
                                      }};

const Scenario cancelledCorrelation{"Cancelled correlation IDs cannot be reused before a late reply", [] {
                                        given("a registry with one cancelled call", [&] {
                                            webfront::detail::PendingCalls<CorrelationPolicy> calls;
                                            auto                                              retained = std::make_shared<int>(42);
                                            std::weak_ptr<int>                                released = retained;
                                            const auto cancelled = calls.add({[retained](const auto&) {}, [retained](auto) {}});
                                            retained.reset();
                                            calls.cancel(cancelled);
                                            check(released.expired());
                                            when("the correlation counter wraps", [&] {
                                                for (std::size_t i = 1; i < std::numeric_limits<msg::CallId>::max(); ++i) {
                                                    const auto id = calls.add({[](const auto&) {}, [](auto) {}});
                                                    require(id != cancelled);
                                                    msg::FunctionReturn<CorrelationPolicy> result;
                                                    result.setCallId(id);
                                                    require(calls.complete(result));
                                                }
                                                const auto next = calls.add({[](const auto&) {}, [](auto) {}});
                                                then("the reserved ID is skipped until its response is consumed", [&] {
                                                    check(next != cancelled);
                                                    msg::FunctionReturn<CorrelationPolicy> late;
                                                    late.setCallId(cancelled);
                                                    check(calls.complete(late));
                                                });
                                            });
                                        });
                                    }};

const Scenario serverScope{"A server scope can release the final owner of a WebSocket during cancellation", [] {
                               InjectableSocket::reset();
                               InlineTaskScope scope;
                               given("a WebSocket owned only by its pending read and write tasks", [&] {
                                   auto socket = websocket::WebSocket<InjectableNetworking>::create(InjectableSocket{}, scope);
                                   std::weak_ptr<websocket::WebSocket<InjectableNetworking>> lifetime = socket;
                                   socket->start();
                                   InjectableSocket::deferWrites();
                                   socket->write(std::string_view("pending"));
                                   socket.reset();
                                   require(!lifetime.expired());
                                   when("the parent scope requests stop inline", [&] {
                                       scope.requestStop();
                                       then("both tasks release the socket before the stop callback returns", [&] {
                                           check(lifetime.expired());
                                           check(InjectableSocket::completeDeferredWrites() == 1);
                                       });
                                   });
                               });
                           }};

const Scenario browserSenders{"Browser senders reject inconsistent return envelopes", [] {
                                  InjectableSocket::reset();
                                  InlineTaskScope               scope;
                                  WebLink<InjectableNetworking> link{InjectableSocket{}, 1, [](WebLinkEvent) {}, scope};
                                  SenderApp                     app{link};
                                  BasicUI<SenderApp>            ui{app, 1, 1};
                                  ex::inplace_stop_source       stop;
                                  SenderObserved                observed;
                                  given("a pending string call", [&] {
                                      auto operation = ex::connect(ui.jsFunctionSender<std::string>("result")(), Receiver{observed, stop.get_token()});
                                      ex::start(operation);
                                      when("a return contains no result", [&] {
                                          reply(1, [](auto&, auto&) {});
                                          then("its arity error reaches the receiver", [&] {
                                              check(observed.error == "Malformed function return message");
                                          });
                                      });
                                      when("a return contains trailing value data", [&] {
                                          reply(1, [](auto& message, auto& frame) {
                                              message.encodeParameter("first", frame);
                                              message.encodeParameter("extra", frame);
                                              message.setParametersCount(1);
                                              frame.freeze();
                                          });
                                          then("the extra bytes are rejected", [&] {
                                              check(observed.error == "Function return contains trailing data");
                                          });
                                      });
                                      when("an encoded exception contains trailing data", [&] {
                                          reply(1, [](auto& message, auto& frame) {
                                              const std::runtime_error error("failure");
                                              message.encodeParameter(error, frame);
                                              message.encodeParameter("extra", frame);
                                              message.setParametersCount(1);
                                              frame.freeze();
                                          });
                                          then("the malformed exception is rejected", [&] {
                                              check(observed.error == "Malformed exception return payload");
                                          });
                                      });
                                      check(observed.completions == 1);
                                  });
                                  given("a pending void call", [&] {
                                      auto operation = ex::connect(ui.jsFunctionSender("finish")(), Receiver{observed, stop.get_token()});
                                      ex::start(operation);
                                      reply(1, [](auto& message, auto& frame) {
                                          message.encodeParameter("unexpected", frame);
                                          frame.freeze();
                                      });
                                      then("a nonempty result is rejected", [&] {
                                          check(observed.error == "Malformed void return message");
                                          check(observed.completions == 1);
                                      });
                                  });
                                  given("a browser reply whose correlation ID was never registered", [&] {
                                      stringReply(99, "unknown");
                                      then("the receive loop still accepts the next valid call", [&] {
                                          auto [id, future] = link.expectResult<std::string>();
                                          stringReply(id, "valid");
                                          check(future.get() == "valid");
                                      });
                                  });
                              }};

const Scenario correlationExhaustion{"Correlation exhaustion fails until an outstanding reply releases an ID", [] {
                                         given("a registry with every ID reserved by an unanswered cancelled call", [&] {
                                             webfront::detail::PendingCalls<CorrelationPolicy> calls;
                                             for (std::size_t i = 0; i < std::numeric_limits<msg::CallId>::max(); ++i) {
                                                 const auto id = calls.add({});
                                                 calls.cancel(id);
                                             }
                                             when("another call is registered", [&] {
                                                 then("registration fails instead of reusing an unanswered ID", [&] {
                                                     checkThrows(
                                                         [&] {
                                                             calls.add({});
                                                         },
                                                         equals("Too many outstanding WebFront calls"));
                                                 });
                                             });
                                             when("the browser returns one of the cancelled calls", [&] {
                                                 msg::FunctionReturn<CorrelationPolicy> result;
                                                 result.setCallId(7);
                                                 require(calls.complete(result));
                                                 then("its ID is available again", [&] {
                                                     check(calls.add({}) == 7);
                                                 });
                                             });
                                         });
                                     }};

const Scenario senderReports{
    "A sender reports a socket expiring independently of its correlation registry",
    [] {
        given("an expired socket and a still-owned registry", [&] {
            auto                   calls = std::make_shared<webfront::detail::PendingCalls<http::DefaultBuffersPolicy>>();
            std::vector<std::byte> payload(sizeof(msg::FunctionCallHeader<msg::Command::callFunction>));
            webfront::detail::BrowserCallSender<InjectableNetworking, http::DefaultBuffersPolicy, std::string> sender{calls, {}, std::move(payload)};
            SenderObserved                                                                                     observed;
            ex::inplace_stop_source                                                                            stop;
            auto operation = ex::connect(std::move(sender), Receiver{observed, stop.get_token()});
            when("the operation starts", [&] {
                ex::start(operation);
                then("the receiver gets a connection error without accessing the socket", [&] {
                    check(observed.error == "Browser connection closed");
                    check(observed.completions == 1);
                });
            });
        });
    }};

const Scenario obsoleteRegistration{"An obsolete registration cannot cancel a replacement with the same wire ID", [] {
                                        given("a completed call whose operation is still delivering an error", [&] {
                                            webfront::detail::PendingCalls<CorrelationPolicy> calls;
                                            int                                               originalOwner{};
                                            int                                               replacementOwner{};
                                            const auto old = calls.add({.complete = [](const auto&) {}, .reject = {}, .owner = &originalOwner});
                                            msg::FunctionReturn<CorrelationPolicy> result;
                                            result.setCallId(old);
                                            require(calls.complete(result));
                                            for (std::size_t i = 1; i < std::numeric_limits<msg::CallId>::max(); ++i) {
                                                result.setCallId(calls.add({}));
                                                require(calls.complete(result));
                                            }
                                            bool       completed   = false;
                                            const auto replacement = calls.add({.complete =
                                                                                    [&](const auto&) {
                                                                                        completed = true;
                                                                                    },
                                                                                .reject = {},
                                                                                .owner  = &replacementOwner});
                                            require(replacement == old);
                                            when("the obsolete operation tries to discard its registration", [&] {
                                                calls.cancel(old, &originalOwner);
                                                result.setCallId(replacement);
                                                require(calls.complete(result));
                                                then("the replacement still receives its own reply", [&] {
                                                    check(completed);
                                                });
                                            });
                                        });
                                    }};
