#include <webfront/dom/Document.hpp>
#include <webfront/testing/Bdd.hpp>

using namespace webfront::bdd;

using namespace webfront;
namespace {
struct Browser {
    std::shared_ptr<dom::detail::State> state;
    auto                                domState() const {
        return state;
    }
};
}  // namespace

const Scenario dOMDocuments{"DOM documents batch writes and preserve node identity", {"dom"}, [] {
                                auto                           state = std::make_shared<dom::detail::State>();
                                dom::Document                  doc{Browser{state}};
                                std::vector<msg::DomOperation> sent;
                                unsigned                       batches{}, wakes{};
                                state->send = [&](const msg::DomBatch& batch) {
                                    ++batches;
                                    sent = msg::DomBatch::decode(batch.data()).second;
                                };
                                state->schedule = [&] {
                                    ++wakes;
                                };
                                given("writes before the interpreter connects", [&] {
                                    auto root    = doc.island();
                                    auto element = doc.element("p");
                                    auto text    = doc.text("été 🌍");
                                    element.append(text);
                                    root.append(element);
                                    doc.flush();
                                    require(batches == 0);
                                    require(wakes == 0);
                                    when("the interpreter connects", [&] {
                                        state->ready = true;
                                        state->wake();
                                        text.setText("updated");
                                        doc.flush();
                                        then("one batch contains the names once and all writes", [&] {
                                            require(wakes == 1);
                                            require(batches == 1);
                                            require(sent.size() == 8);
                                            require(sent[0].code == msg::DomOp::name);
                                            require(sent[0].value == "root");
                                            require(sent.back().code == msg::DomOp::setText);
                                            require(sent.back().value == "updated");
                                            doc.flush();
                                            require(batches == 1);
                                        });
                                    });
                                    when("the application attempts to replace element children through setText", [&] {
                                        requireThrows<std::invalid_argument>([&] {
                                            element.setText("replacement");
                                        });
                                        require(text.valid());
                                    });
                                    when("a subtree is removed", [&] {
                                        element.remove();
                                        require(!element.valid());
                                        require(!text.valid());
                                        require(root.valid());
                                        requireThrows([&] {
                                            text.setText("stale");
                                        });
                                    });
                                    when("structure would create a cycle or move an island", [&] {
                                        requireThrows<std::invalid_argument>([&] {
                                            element.append(root);
                                        });
                                        requireThrows<std::invalid_argument>([&] {
                                            text.append(element);
                                        });
                                        auto child = doc.element("div");
                                        element.append(child);
                                        requireThrows<std::invalid_argument>([&] {
                                            child.append(element);
                                        });
                                    });
                                });
                            }};

const Scenario dOMEvents{"DOM events and island revocation release callbacks", {"dom"}, [] {
                             auto state   = std::make_shared<dom::detail::State>();
                             state->ready = true;
                             state->send  = [](const auto&) {};
                             dom::Document doc{Browser{state}};
                             auto          root   = doc.island("panel");
                             auto          other  = doc.island("other");
                             auto          button = doc.element("button");
                             root.append(button);
                             int calls{};
                             button.listen("click",
                                           [&](std::string value) {
                                               require(value.empty());
                                               ++calls;
                                               button.unlisten("click");
                                           },
                                           {true, true});
                             auto event = state->names.at("click");
                             doc.flush();
                             state->event(button.id(), event, "");
                             state->event(button.id(), event, "");
                             require(calls == 1);
                             doc.flush();
                             const auto oldRootId = root.id();
                             state->revoke("panel");
                             require(!root.valid());
                             require(!button.valid());
                             require(other.valid());
                             auto newRoot = doc.island("panel");
                             require(newRoot.id() != oldRootId);
                             requireThrows([&] {
                                 root.id();
                             });
                         }};

const Scenario dOMConnections{"DOM connections have separate lifetimes and closed handles remain safe", {"dom"}, [] {
                                  auto          first  = std::make_shared<dom::detail::State>();
                                  auto          second = std::make_shared<dom::detail::State>();
                                  dom::Document a{Browser{first}}, b{Browser{second}};
                                  auto          node  = a.text("first");
                                  auto          other = b.element("div");
                                  requireThrows<std::invalid_argument>([&] {
                                      other.append(node);
                                  });
                                  bool rejected{};
                                  a.onError([&](std::uint32_t id, std::string reason) {
                                      rejected = id == 1 && reason == "rejected";
                                  });
                                  first->reject(1, "rejected");
                                  require(rejected);
                                  require(!a.valid());
                                  require(!node.valid());
                                  require(other.valid());
                                  requireThrows([&] {
                                      a.text("late");
                                  });
                                  second->close();  // Same invalidation used on disconnect and shutdown.
                                  require(!other.valid());
                                  requireThrows([&] {
                                      other.remove();
                                  });
                              }};

const Scenario dOMRejects{"DOM rejects off-thread access and retains a bounded pending batch on errors", {"dom"}, [] {
                              auto          state = std::make_shared<dom::detail::State>();
                              dom::Document doc{Browser{state}};
                              bool          rejected{};
                              std::thread   thread([&] {
                                  try {
                                      doc.text("wrong thread");
                                  } catch (const std::logic_error&) {
                                      rejected = true;
                                  }
                              });
                              thread.join();
                              require(rejected);
                              auto       text = doc.text("kept");
                              const auto size = state->pending.data().size();
                              requireThrows<std::length_error>([&] {
                                  text.setText(std::string(msg::DomBatch::maxBytes, 'x'));
                              });
                              require(state->pending.data().size() == size);
                              require(text.valid());
                              requireThrows<std::length_error>([&] {
                                  doc.element(std::string(1025, 'x'));
                              });
                          }};

namespace {
struct ReadBrowser : Browser {
    std::vector<std::uint8_t> result;
    std::shared_ptr<unsigned> starts{std::make_shared<unsigned>(0)};
    template <typename R>
    auto jsFunctionSender(std::string_view) const {
        static_assert(std::is_same_v<R, std::vector<std::uint8_t>>);
        return [bytes = result, calls = starts](const auto&) {
            return networking::ex::then(networking::ex::just(), [bytes, calls] {
                ++*calls;
                return bytes;
            });
        };
    }
};
}  // namespace
const Scenario dOMGrouped{"DOM grouped read results are bounded and structurally exact", {"dom"}, [] {
                              auto state   = std::make_shared<dom::detail::State>();
                              state->ready = true;
                              state->send  = [](const auto&) {};
                              ReadBrowser browser{
                                  {state},
                                  {3, 0, 0, 0, 'o', 'n', 'e', 0, 0, 0, 0}
                              };
                              dom::Document doc{browser};
                              auto          first = doc.text("one"), second = doc.text("");
                              auto          sender = doc.read({
                                  { first, dom::Read::text},
                                  {second, dom::Read::text}
                              });
                              const auto    result = networking::ex::sync_wait(std::move(sender));
                              require(std::get<0>(*result) == std::vector<std::string>{"one", ""});
                              for (auto bytes : std::vector<std::vector<std::uint8_t>>{
                                       {},
                                       {1, 0, 0},
                                       {5, 0, 0, 0, 'a'},
                                       {0, 0, 0, 0, 1},
                                       {2, 0, 0, 0, 0xc0, 0x80}
                              }) {
                                  browser.result = bytes;
                                  dom::Document malformed{browser};
                                  requireThrows([&] {
                                      networking::ex::sync_wait(malformed.read({
                                          {first, dom::Read::text}
                                      }));
                                  });
                              }
                              requireThrows<std::length_error>([&] {
                                  doc.read({});
                              });
                              state->close();
                              requireThrows([&] {
                                  networking::ex::sync_wait(doc.read({
                                      {first, dom::Read::text}
                                  }));
                              });
                          }};

const Scenario dOMRejectsElement{"DOM rejects element-only operations before changing a pending batch", {"dom"}, [] {
                                     auto          state = std::make_shared<dom::detail::State>();
                                     dom::Document doc{Browser{state}};
                                     auto          text = doc.text("text"), island = doc.island();
                                     for (const auto& node : {text, island}) {
                                         const auto size  = state->pending.data().size();
                                         const auto names = state->names.size();
                                         requireThrows<std::invalid_argument>([&] {
                                             node.attribute("title", "value");
                                         });
                                         requireThrows<std::invalid_argument>([&] {
                                             node.unsetAttribute("title");
                                         });
                                         requireThrows<std::invalid_argument>([&] {
                                             node.property("value", "value");
                                         });
                                         requireThrows<std::invalid_argument>([&] {
                                             node.listen("click", [](std::string) {});
                                         });
                                         requireThrows<std::invalid_argument>([&] {
                                             node.unlisten("click");
                                         });
                                         require(state->pending.data().size() == size);
                                         require(state->names.size() == names);
                                         require(state->listeners.empty());
                                         require(node.valid());
                                     }
                                     require(doc.valid());
                                 }};

const Scenario dOMReports{"DOM reports batch identifier exhaustion exactly once", {"dom"}, [] {
                              auto state     = std::make_shared<dom::detail::State>();
                              state->batchId = std::numeric_limits<std::uint32_t>::max();
                              state->pending = msg::DomBatch(state->batchId);
                              state->ready   = true;
                              unsigned sent{}, errors{};
                              state->send = [&](const msg::DomBatch& batch) {
                                  require(msg::DomBatch::decode(batch.data()).first == state->batchId);
                                  ++sent;
                              };
                              dom::Document doc{Browser{state}};
                              doc.onError([&](std::uint32_t id, std::string reason) {
                                  require(id == std::numeric_limits<std::uint32_t>::max());
                                  require(reason == "DOM batch identifiers exhausted");
                                  ++errors;
                              });
                              auto node = doc.text("last batch");
                              requireThrows<std::length_error>([&] {
                                  doc.flush();
                              });
                              require(sent == 1);
                              require(errors == 1);
                              require(!doc.valid());
                              require(!node.valid());
                          }};

const Scenario insertingDOM{"Inserting a DOM node before itself preserves its identity and pending batch", {"dom"}, [] {
                                auto          state = std::make_shared<dom::detail::State>();
                                dom::Document doc{Browser{state}};
                                auto          parent = doc.element("div"), child = doc.text("child");
                                parent.append(child);
                                const auto size = state->pending.data().size();
                                parent.insertBefore(child, child);
                                require(state->pending.data().size() == size);
                                require(child.valid());
                                require(state->get(child.id()).parent == parent.id());
                            }};

const Scenario dOMValidity{"DOM validity predicates are nonthrowing and agree about the calling thread", {"dom"}, [] {
                               auto          state = std::make_shared<dom::detail::State>();
                               dom::Document doc{Browser{state}};
                               auto          node = doc.text("valid");
                               static_assert(noexcept(doc.valid()));
                               static_assert(noexcept(node.valid()));
                               require(doc.valid());
                               require(node.valid());
                               bool        nodeValid = true, documentValid = true, threw = false;
                               std::thread caller([&] {
                                   try {
                                       nodeValid     = node.valid();
                                       documentValid = doc.valid();
                                   } catch (...) {
                                       threw = true;
                                   }
                               });
                               caller.join();
                               require(!threw);
                               require(!nodeValid);
                               require(!documentValid);
                               require(doc.valid());
                               require(node.valid());
                               node.remove();
                               require(!node.valid());
                               require(doc.valid());
                               auto moved = std::move(doc);
                               require(!doc.valid());
                               require(moved.valid());
                               state->close();
                               require(!doc.valid());
                               require(!moved.valid());
                               require(!node.valid());
                           }};

const Scenario dOMRejectsEmpty{"DOM rejects empty event callbacks without changing subscriptions or pending names", {"dom"}, [] {
                                   auto          state = std::make_shared<dom::detail::State>();
                                   dom::Document doc{Browser{state}};
                                   auto          button = doc.element("button");
                                   unsigned      calls{};
                                   button.listen("click", [&](std::string) {
                                       ++calls;
                                   });
                                   const auto size  = state->pending.data().size();
                                   const auto names = state->names.size();
                                   requireThrows<std::invalid_argument>([&] {
                                       button.listen("keydown", {});
                                   });
                                   requireThrows<std::invalid_argument>([&] {
                                       button.listen("click", {});
                                   });
                                   require(state->pending.data().size() == size);
                                   require(state->names.size() == names);
                                   require(state->listeners.size() == 1);
                                   state->event(button.id(), state->names.at("click"), "");
                                   require(calls == 1);
                               }};

const Scenario islandUnmount{"An island unmount with pending writes closes the entire document once", {"dom"}, [] {
                                 auto state   = std::make_shared<dom::detail::State>();
                                 state->ready = true;
                                 state->send  = [](const auto&) {};
                                 dom::Document doc{Browser{state}};
                                 auto          first = doc.island("first"), second = doc.island("second");
                                 auto          text = doc.text("second island");
                                 second.append(text);
                                 doc.flush();
                                 unsigned errors{};
                                 doc.onError([&](std::uint32_t, std::string reason) {
                                     require(reason == "DOM island revoked with pending writes");
                                     ++errors;
                                 });
                                 text.setText("pending in a different island");
                                 state->revoke("first");
                                 require(errors == 1);
                                 require(!doc.valid());
                                 require(!first.valid());
                                 require(!second.valid());
                                 require(!text.valid());
                             }};


const Scenario creatingGrouped{"Creating a grouped DOM read flushes writes even when its lazy read sender is discarded", {"dom"}, [] {
                                   auto state   = std::make_shared<dom::detail::State>();
                                   state->ready = true;
                                   unsigned writes{};
                                   state->send = [&](const auto&) {
                                       ++writes;
                                   };
                                   ReadBrowser browser{
                                       {state},
                                       {0, 0, 0, 0}
                                   };
                                   dom::Document doc{browser};
                                   auto          text = doc.text("pending write");
                                   require(writes == 0);
                                   static_cast<void>(doc.read({
                                       {text, dom::Read::text}
                                   }));
                                   require(writes == 1);
                                   require(*browser.starts == 0);
                                   auto sender = doc.read({
                                       {text, dom::Read::text}
                                   });
                                   require(writes == 1);
                                   require(*browser.starts == 0);
                                   require(networking::ex::sync_wait(std::move(sender)).has_value());
                                   require(*browser.starts == 1);
                               }};

namespace {
namespace ex = networking::ex;
struct Awaited {
    unsigned                   completions{};
    bool                       stopped{};
    std::optional<std::string> value;
    std::string                error;
};
struct EventReceiver {
    using receiver_concept = ex::receiver_tag;
    Awaited*               awaited;
    ex::inplace_stop_token token;
    auto                   get_env() const noexcept {
        return ex::prop{ex::get_stop_token, token};
    }
    void set_value(std::string value) && noexcept {
        awaited->value = std::move(value);
        ++awaited->completions;
    }
    void set_stopped() && noexcept {
        awaited->stopped = true;
        ++awaited->completions;
    }
    void set_error(std::exception_ptr e) && noexcept {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& error) {
            awaited->error = error.what();
        }
        ++awaited->completions;
    }
};
std::vector<msg::DomOp> pendingCodes(const dom::detail::State& state) {
    std::vector<msg::DomOp> codes;
    for (const auto& operation : msg::DomBatch::decode(state.pending.data()).second)
        codes.push_back(operation.code);
    return codes;
}
struct ReadyDocument {
    std::shared_ptr<dom::detail::State> state{std::make_shared<dom::detail::State>()};
    dom::Document<Browser>              doc{Browser{state}};
    ReadyDocument() {
        state->ready = true;
        state->send  = [](const auto&) {};
    }
};
}  // namespace

const Scenario awaitedDOM{"An awaited DOM event subscribes when started and delivers one value", {"dom"}, [] {
                              ReadyDocument document;
                              auto&         state = document.state;
                              auto          input = document.doc.element("input");
                              document.doc.island().append(input);
                              document.doc.flush();
                              given("a sender of the next input event", [&] {
                                  auto sender = input.next("input", {.preventDefault = true, .stopPropagation = false});
                                  then("creating it subscribes nothing", [&] {
                                      require(pendingCodes(*state).empty());
                                      require(state->waiters.empty());
                                  });
                                  when("it is started", [&] {
                                      Awaited awaited;
                                      auto    operation = ex::connect(sender, EventReceiver{&awaited, {}});
                                      ex::start(operation);
                                      const auto name = state->names.at("input");
                                      then("the browser subscription carries the flags declared by next()", [&] {
                                          const auto operations = msg::DomBatch::decode(state->pending.data()).second;
                                          require(operations.back().code == msg::DomOp::listen);
                                          require(operations.back().a == input.id());
                                          require(operations.back().b == name);
                                          require(operations.back().c == 1);
                                          require(awaited.completions == 0);
                                      });
                                      andWhen("the event arrives twice", [&] {
                                          document.doc.flush();
                                          state->event(input.id(), name, "first");
                                          state->event(input.id(), name, "second");
                                          then("only the first completes the wait, which unsubscribes", [&] {
                                              require(awaited.completions == 1);
                                              require(awaited.value == "first");
                                              require(pendingCodes(*state) == std::vector{msg::DomOp::unlisten});
                                              require(state->waiters.empty());
                                          });
                                          andThen("the same sender waits again for a later event", [&] {
                                              Awaited again;
                                              auto    next = ex::connect(sender, EventReceiver{&again, {}});
                                              ex::start(next);
                                              state->event(input.id(), name, "third");
                                              require(again.value == "third");
                                              require(pendingCodes(*state) == std::vector{msg::DomOp::unlisten, msg::DomOp::listen, msg::DomOp::unlisten});
                                          });
                                      });
                                  });
                              });
                          }};

const Scenario awaitedDOMEvent{"An awaited DOM event completes stopped when it can no longer occur", {"dom"}, [] {
                                   ReadyDocument document;
                                   auto&         state  = document.state;
                                   auto          island = document.doc.island("panel");
                                   auto          button = document.doc.element("button");
                                   island.append(button);
                                   document.doc.flush();
                                   Awaited awaited;
                                   auto    operation = ex::connect(button.next("click"), EventReceiver{&awaited, {}});
                                   ex::start(operation);
                                   require(awaited.completions == 0);
                                   when("the application unsubscribes the event", [&] {
                                       button.unlisten("click");
                                       then("the wait stops", [&] {
                                           require(awaited.stopped);
                                       });
                                   });
                                   when("the node is removed", [&] {
                                       button.remove();
                                       then("the wait stops", [&] {
                                           require(awaited.stopped);
                                       });
                                   });
                                   when("the page revokes the island", [&] {
                                       state->revoke("panel");
                                       then("the wait stops", [&] {
                                           require(awaited.stopped);
                                           require(!button.valid());
                                       });
                                   });
                                   when("the browser disconnects", [&] {
                                       state->close();
                                       then("the wait stops", [&] {
                                           require(awaited.stopped);
                                       });
                                   });
                                   when("the browser rejects a batch", [&] {
                                       bool reported{};
                                       document.doc.onError([&](std::uint32_t, std::string) {
                                           reported = awaited.stopped;
                                       });
                                       state->reject(1, "rejected");
                                       then("the wait stops before the error is reported", [&] {
                                           require(awaited.stopped);
                                           require(reported);
                                       });
                                   });
                                   require(awaited.completions == 1);
                               }};

const Scenario awaitedDOMEventCancelled{"An awaited DOM event is cancelled through its stop token", {"dom"}, [] {
                                            ReadyDocument           document;
                                            auto&                   state  = document.state;
                                            auto                    button = document.doc.element("button");
                                            ex::inplace_stop_source source;
                                            Awaited                 awaited;
                                            given("a stop requested before start", [&] {
                                                source.request_stop();
                                                auto operation = ex::connect(button.next("click"), EventReceiver{&awaited, source.get_token()});
                                                ex::start(operation);
                                                then("it completes stopped without subscribing", [&] {
                                                    require(awaited.stopped);
                                                    require(awaited.completions == 1);
                                                    require(state->waiters.empty());
                                                    require(!state->names.contains("click"));
                                                });
                                            });
                                            given("a stop requested from another thread while waiting", [&] {
                                                auto operation = ex::connect(button.next("click"), EventReceiver{&awaited, source.get_token()});
                                                ex::start(operation);
                                                std::thread([&] {
                                                    source.request_stop();
                                                }).join();
                                                require(awaited.stopped);
                                                const auto name = state->names.at("click");
                                                when("the event arrives afterwards", [&] {
                                                    state->event(button.id(), name, "");
                                                    then("the obsolete subscription is released without a second completion", [&] {
                                                        require(awaited.completions == 1);
                                                        require(state->waiters.empty());
                                                        require(pendingCodes(*state).back() == msg::DomOp::unlisten);
                                                    });
                                                });
                                                when("a new wait or a callback subscribes the same event", [&] {
                                                    Awaited again;
                                                    auto    next = ex::connect(button.next("click"), EventReceiver{&again, {}});
                                                    ex::start(next);
                                                    state->event(button.id(), name, "");
                                                    require(again.value == "");
                                                    button.listen("click", [](std::string) {});
                                                    then("the obsolete wait does not conflict", [&] {
                                                        require(again.completions == 1);
                                                        require(state->listeners.size() == 1);
                                                    });
                                                });
                                            });
                                        }};

const Scenario awaitedDOMEventRejects{"An awaited DOM event rejects conflicting subscriptions and invalid nodes", {"dom"}, [] {
                                          ReadyDocument document;
                                          auto&         state  = document.state;
                                          auto          button = document.doc.element("button");
                                          auto          text   = document.doc.text("text");
                                          requireThrows<std::invalid_argument>([&] {
                                              (void)text.next("click");
                                          });
                                          requireThrows<std::invalid_argument>([&] {
                                              (void)document.doc.island().next("click");
                                          });
                                          Awaited first;
                                          auto    operation = ex::connect(button.next("click"), EventReceiver{&first, {}});
                                          ex::start(operation);
                                          const auto size = state->pending.data().size();
                                          when("a second wait starts for the same event", [&] {
                                              Awaited second;
                                              auto    other = ex::connect(button.next("click"), EventReceiver{&second, {}});
                                              ex::start(other);
                                              then("only the second fails, without changing the pending batch", [&] {
                                                  require(second.error == "DOM event is already awaited");
                                                  require(first.completions == 0);
                                                  require(state->pending.data().size() == size);
                                              });
                                          });
                                          when("a callback subscribes the awaited event", [&] {
                                              requireThrows<std::logic_error>([&] {
                                                  button.listen("click", [](std::string) {});
                                              });
                                              then("the wait still receives it", [&] {
                                                  require(state->pending.data().size() == size);
                                                  state->event(button.id(), state->names.at("click"), "");
                                                  require(first.value == "");
                                              });
                                          });
                                          when("a wait starts for an event with a callback", [&] {
                                              button.listen("keydown", [](std::string) {});
                                              Awaited second;
                                              auto    other = ex::connect(button.next("keydown"), EventReceiver{&second, {}});
                                              ex::start(other);
                                              then("it fails", [&] {
                                                  require(second.error == "DOM event already has a listener");
                                              });
                                          });
                                          when("a wait starts on another thread", [&] {
                                              Awaited second;
                                              auto    other = ex::connect(button.next("input"), EventReceiver{&second, {}});
                                              std::thread([&] {
                                                  ex::start(other);
                                              }).join();
                                              then("it fails without touching the document", [&] {
                                                  require(second.error == "DOM document requires the server thread");
                                                  require(!state->names.contains("input"));
                                              });
                                          });
                                          when("the node is removed before a wait starts", [&] {
                                              auto sender = button.next("input");
                                              button.remove();
                                              Awaited second;
                                              auto    other = ex::connect(sender, EventReceiver{&second, {}});
                                              ex::start(other);
                                              then("it fails", [&] {
                                                  require(second.error == "DOM node is invalid");
                                              });
                                          });
                                      }};

namespace {
Task wizard(dom::Node next, dom::Node back, std::vector<std::string>& steps) {
    steps.push_back("name: " + co_await next.next("click"));
    steps.push_back("back: " + co_await back.next("click"));
    steps.push_back("done: " + co_await next.next("click"));
}
// Each click follows a hop on the shared scheduler, after the wizard resumed and waits again.
Task clicks(std::shared_ptr<dom::detail::State> state, std::vector<std::pair<dom::Node, std::string>> sequence) {
    auto scheduler = co_await ex::read_env(ex::get_scheduler);
    for (auto& [node, value] : sequence) {
        co_await ex::schedule(scheduler);
        state->event(node.id(), state->names.at("click"), std::move(value));
    }
}
}  // namespace

const Scenario coroutineState{"A coroutine is the state machine of a sequential DOM dialog", {"dom"}, [] {
                                  ReadyDocument                                  document;
                                  auto                                           next = document.doc.element("button");
                                  auto                                           back = document.doc.element("button");
                                  std::vector<std::string>                       steps;
                                  std::vector<std::pair<dom::Node, std::string>> sequence{
                                      {back, "ignored"},
                                      {next,       "1"},
                                      {back,       "2"},
                                      {next,       "3"}
                                  };
#if defined(_MSC_VER) && _MSC_VER >= 1952
                                  // MSVC 19.52 rejects stdexec's when_all with two coroutine task senders.
                                  // Exercise both coroutines on the server's shared scheduler instead.
                                  ::stdnet::io_context  context;
                                  networking::TaskScope scope;
                                  const auto            scheduler     = context.get_scheduler();
                                  const bool            wizardStarted = networking::Spawn(scope, scheduler, wizard(next, back, steps));
                                  const bool            clicksStarted = networking::Spawn(scope, scheduler, clicks(document.state, std::move(sequence)));
                                  context.run();
                                  scope.drain(context);
                                  require(wizardStarted && clicksStarted);
#else
    ex::sync_wait(ex::when_all(wizard(next, back, steps), clicks(document.state, std::move(sequence))));
#endif
                                  require(steps == std::vector<std::string>{"name: 1", "back: 2", "done: 3"});
                                  require(document.state->waiters.empty());
                              }};

namespace {
// Fills the pending batch until not even an unsubscription fits.
void fillBatch(dom::detail::State& state, std::uint32_t node) {
    try {
        for (;;)
            state.add({msg::DomOp::unlisten, node, 0});
    } catch (const std::length_error&) {}
}
}  // namespace

const Scenario cancelledDOM{"A cancelled DOM event wait releases its browser subscription", {"dom"}, [] {
                                ReadyDocument           document;
                                auto&                   state  = document.state;
                                auto                    button = document.doc.element("button");
                                ex::inplace_stop_source source;
                                Awaited                 awaited;
                                auto                    operation = ex::connect(button.next("click", {.preventDefault = true, .stopPropagation = true}),
                                                             EventReceiver{&awaited, source.get_token()});
                                ex::start(operation);
                                const auto name = state->names.at("click");
                                document.doc.flush();
                                given("a stop requested on the server thread", [&] {
                                    source.request_stop();
                                    then("the wait stops and its browser listener is removed at once", [&] {
                                        require(awaited.stopped);
                                        require(state->waiters.empty());
                                        const auto operations = msg::DomBatch::decode(state->pending.data()).second;
                                        require(operations.size() == 1);
                                        require(operations[0].code == msg::DomOp::unlisten);
                                        require(operations[0].a == button.id());
                                        require(operations[0].b == name);
                                    });
                                    andWhen("the event was already on its way", [&] {
                                        state->event(button.id(), name, "");
                                        then("it is ignored", [&] {
                                            require(awaited.completions == 1);
                                            require(pendingCodes(*state) == std::vector{msg::DomOp::unlisten});
                                        });
                                    });
                                });
                                given("stops requested from another thread", [&] {
                                    unsigned posts{};
                                    state->post = [&] {
                                        ++posts;
                                    };
                                    ex::inplace_stop_source otherSource;
                                    Awaited                 other;
                                    auto                    second = ex::connect(button.next("keydown"), EventReceiver{&other, otherSource.get_token()});
                                    ex::start(second);
                                    document.doc.flush();
                                    std::thread([&] {
                                        source.request_stop();
                                        otherSource.request_stop();
                                    }).join();
                                    then("one release is posted to the server context", [&] {
                                        require(awaited.stopped);
                                        require(other.stopped);
                                        require(posts == 1);
                                        require(pendingCodes(*state).empty());
                                    });
                                    andWhen("the server context runs it", [&] {
                                        state->sweep();
                                        then("both browser listeners are removed", [&] {
                                            require(state->waiters.empty());
                                            require(pendingCodes(*state) == std::vector{msg::DomOp::unlisten, msg::DomOp::unlisten});
                                        });
                                    });
                                    andWhen("a new wait subscribes the same event before it runs", [&] {
                                        Awaited again;
                                        auto    next = ex::connect(button.next("click"), EventReceiver{&again, {}});
                                        ex::start(next);
                                        state->sweep();
                                        then("the new subscription is kept", [&] {
                                            require(pendingCodes(*state) == std::vector{msg::DomOp::listen, msg::DomOp::unlisten});
                                            state->event(button.id(), name, "later");
                                            require(again.value == "later");
                                        });
                                    });
                                });
                                given("a document closed before a stop from another thread", [&] {
                                    unsigned posts{};
                                    state->post = [&] {
                                        ++posts;
                                    };
                                    state->close();
                                    then("the wait stopped with the document and nothing is posted", [&] {
                                        require(awaited.stopped);
                                        std::thread([&] {
                                            source.request_stop();
                                        }).join();
                                        require(posts == 0);
                                        require(awaited.completions == 1);
                                    });
                                });
                            }};

const Scenario dOMUnsubscription{"A DOM unsubscription that does not fit a full batch opens the next one", {"dom"}, [] {
                                     ReadyDocument           document;
                                     auto&                   state  = document.state;
                                     auto                    button = document.doc.element("button");
                                     ex::inplace_stop_source source;
                                     Awaited                 awaited;
                                     auto                    operation = ex::connect(button.next("click", {.preventDefault = true, .stopPropagation = false}),
                                                                  EventReceiver{&awaited, source.get_token()});
                                     ex::start(operation);
                                     const auto name = state->names.at("click");
                                     document.doc.flush();
                                     fillBatch(*state, button.id());
                                     when("the awaited event arrives", [&] {
                                         state->event(button.id(), name, "value");
                                         require(awaited.value == "value");
                                     });
                                     when("the wait is cancelled", [&] {
                                         source.request_stop();
                                         require(awaited.stopped);
                                     });
                                     require(state->waiters.empty());
                                     document.doc.flush();
                                     const auto operations = msg::DomBatch::decode(state->pending.data()).second;
                                     require(operations.size() == 1);
                                     require(operations[0].code == msg::DomOp::unlisten);
                                     require(operations[0].a == button.id());
                                     require(operations[0].b == name);
                                 }};

const Scenario dOMEvent{"A DOM event wait that cannot be queued subscribes nothing", {"dom"}, [] {
                            ReadyDocument document;
                            auto&         state  = document.state;
                            auto          button = document.doc.element("button");
                            state->intern("click");
                            fillBatch(*state, button.id());
                            Awaited awaited;
                            auto    operation = ex::connect(button.next("click"), EventReceiver{&awaited, {}});
                            ex::start(operation);
                            then("it fails without a waiter", [&] {
                                require(!awaited.error.empty());
                                require(state->waiters.empty());
                            });
                        }};
