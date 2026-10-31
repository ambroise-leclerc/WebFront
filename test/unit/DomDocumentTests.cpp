#include <catch2/catch_test_macros.hpp>
#include <webfront/dom/Document.hpp>

using namespace webfront;
namespace {
struct Browser {
    std::shared_ptr<dom::detail::State> state;
    auto                                domState() const {
        return state;
    }
};
}  // namespace

SCENARIO("DOM documents batch writes and preserve node identity", "[dom]") {
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
    GIVEN("writes before the interpreter connects") {
        auto root    = doc.island();
        auto element = doc.element("p");
        auto text    = doc.text("été 🌍");
        element.append(text);
        root.append(element);
        doc.flush();
        REQUIRE(batches == 0);
        REQUIRE(wakes == 0);
        WHEN("the interpreter connects") {
            state->ready = true;
            state->wake();
            text.setText("updated");
            doc.flush();
            THEN("one batch contains the names once and all writes") {
                REQUIRE(wakes == 1);
                REQUIRE(batches == 1);
                REQUIRE(sent.size() == 8);
                REQUIRE(sent[0].code == msg::DomOp::name);
                REQUIRE(sent[0].value == "root");
                REQUIRE(sent.back().code == msg::DomOp::setText);
                REQUIRE(sent.back().value == "updated");
                doc.flush();
                REQUIRE(batches == 1);
            }
        }
        WHEN("the application attempts to replace element children through setText") {
            REQUIRE_THROWS_AS(element.setText("replacement"), std::invalid_argument);
            REQUIRE(text.valid());
        }
        WHEN("a subtree is removed") {
            element.remove();
            REQUIRE_FALSE(element.valid());
            REQUIRE_FALSE(text.valid());
            REQUIRE(root.valid());
            REQUIRE_THROWS(text.setText("stale"));
        }
        WHEN("structure would create a cycle or move an island") {
            REQUIRE_THROWS_AS(element.append(root), std::invalid_argument);
            REQUIRE_THROWS_AS(text.append(element), std::invalid_argument);
            auto child = doc.element("div");
            element.append(child);
            REQUIRE_THROWS_AS(child.append(element), std::invalid_argument);
        }
    }
}

SCENARIO("DOM events and island revocation release callbacks", "[dom]") {
    auto state   = std::make_shared<dom::detail::State>();
    state->ready = true;
    state->send  = [](const auto&) { };
    dom::Document doc{Browser{state}};
    auto          root   = doc.island("panel");
    auto          other  = doc.island("other");
    auto          button = doc.element("button");
    root.append(button);
    int calls{};
    button.listen("click",
                  [&](std::string value) {
                      REQUIRE(value.empty());
                      ++calls;
                      button.unlisten("click");
                  },
                  {true, true});
    auto event = state->names.at("click");
    doc.flush();
    state->event(button.id(), event, "");
    state->event(button.id(), event, "");
    REQUIRE(calls == 1);
    doc.flush();
    const auto oldRootId = root.id();
    state->revoke("panel");
    REQUIRE_FALSE(root.valid());
    REQUIRE_FALSE(button.valid());
    REQUIRE(other.valid());
    auto newRoot = doc.island("panel");
    REQUIRE(newRoot.id() != oldRootId);
    REQUIRE_THROWS(root.id());
}

SCENARIO("DOM connections have separate lifetimes and closed handles remain safe", "[dom]") {
    auto          first  = std::make_shared<dom::detail::State>();
    auto          second = std::make_shared<dom::detail::State>();
    dom::Document a{Browser{first}}, b{Browser{second}};
    auto          node  = a.text("first");
    auto          other = b.element("div");
    REQUIRE_THROWS_AS(other.append(node), std::invalid_argument);
    bool rejected{};
    a.onError([&](std::uint32_t id, std::string reason) {
        rejected = id == 1 && reason == "rejected";
    });
    first->reject(1, "rejected");
    REQUIRE(rejected);
    REQUIRE_FALSE(a.valid());
    REQUIRE_FALSE(node.valid());
    REQUIRE(other.valid());
    REQUIRE_THROWS(a.text("late"));
    second->close();  // Same invalidation used on disconnect and shutdown.
    REQUIRE_FALSE(other.valid());
    REQUIRE_THROWS(other.remove());
}

SCENARIO("DOM rejects off-thread access and retains a bounded pending batch on errors", "[dom]") {
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
    REQUIRE(rejected);
    auto       text = doc.text("kept");
    const auto size = state->pending.data().size();
    REQUIRE_THROWS_AS(text.setText(std::string(msg::DomBatch::maxBytes, 'x')), std::length_error);
    REQUIRE(state->pending.data().size() == size);
    REQUIRE(text.valid());
    REQUIRE_THROWS_AS(doc.element(std::string(1025, 'x')), std::length_error);
}

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
SCENARIO("DOM grouped read results are bounded and structurally exact", "[dom]") {
    auto state   = std::make_shared<dom::detail::State>();
    state->ready = true;
    state->send  = [](const auto&) { };
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
    REQUIRE(std::get<0>(*result) == std::vector<std::string>{"one", ""});
    for (auto bytes : std::vector<std::vector<std::uint8_t>>{
             {},
             {1, 0, 0},
             {5, 0, 0, 0, 'a'},
             {0, 0, 0, 0, 1},
             {2, 0, 0, 0, 0xc0, 0x80}
    }) {
        browser.result = bytes;
        dom::Document malformed{browser};
        REQUIRE_THROWS(networking::ex::sync_wait(malformed.read({
            {first, dom::Read::text}
        })));
    }
    REQUIRE_THROWS_AS(doc.read({}), std::length_error);
    state->close();
    REQUIRE_THROWS(networking::ex::sync_wait(doc.read({
        {first, dom::Read::text}
    })));
}

SCENARIO("DOM rejects element-only operations before changing a pending batch", "[dom]") {
    auto          state = std::make_shared<dom::detail::State>();
    dom::Document doc{Browser{state}};
    auto          text = doc.text("text"), island = doc.island();
    for (const auto& node : {text, island}) {
        const auto size  = state->pending.data().size();
        const auto names = state->names.size();
        REQUIRE_THROWS_AS(node.attribute("title", "value"), std::invalid_argument);
        REQUIRE_THROWS_AS(node.unsetAttribute("title"), std::invalid_argument);
        REQUIRE_THROWS_AS(node.property("value", "value"), std::invalid_argument);
        REQUIRE_THROWS_AS(node.listen("click", [](std::string) { }), std::invalid_argument);
        REQUIRE_THROWS_AS(node.unlisten("click"), std::invalid_argument);
        REQUIRE(state->pending.data().size() == size);
        REQUIRE(state->names.size() == names);
        REQUIRE(state->listeners.empty());
        REQUIRE(node.valid());
    }
    REQUIRE(doc.valid());
}

SCENARIO("DOM reports batch identifier exhaustion exactly once", "[dom]") {
    auto state     = std::make_shared<dom::detail::State>();
    state->batchId = std::numeric_limits<std::uint32_t>::max();
    state->pending = msg::DomBatch(state->batchId);
    state->ready   = true;
    unsigned sent{}, errors{};
    state->send = [&](const msg::DomBatch& batch) {
        REQUIRE(msg::DomBatch::decode(batch.data()).first == state->batchId);
        ++sent;
    };
    dom::Document doc{Browser{state}};
    doc.onError([&](std::uint32_t id, std::string reason) {
        REQUIRE(id == std::numeric_limits<std::uint32_t>::max());
        REQUIRE(reason == "DOM batch identifiers exhausted");
        ++errors;
    });
    auto node = doc.text("last batch");
    REQUIRE_THROWS_AS(doc.flush(), std::length_error);
    REQUIRE(sent == 1);
    REQUIRE(errors == 1);
    REQUIRE_FALSE(doc.valid());
    REQUIRE_FALSE(node.valid());
}

SCENARIO("Inserting a DOM node before itself preserves its identity and pending batch", "[dom]") {
    auto          state = std::make_shared<dom::detail::State>();
    dom::Document doc{Browser{state}};
    auto          parent = doc.element("div"), child = doc.text("child");
    parent.append(child);
    const auto size = state->pending.data().size();
    parent.insertBefore(child, child);
    REQUIRE(state->pending.data().size() == size);
    REQUIRE(child.valid());
    REQUIRE(state->get(child.id()).parent == parent.id());
}

SCENARIO("DOM validity predicates are nonthrowing and agree about the calling thread", "[dom]") {
    auto          state = std::make_shared<dom::detail::State>();
    dom::Document doc{Browser{state}};
    auto          node = doc.text("valid");
    STATIC_REQUIRE(noexcept(doc.valid()));
    STATIC_REQUIRE(noexcept(node.valid()));
    REQUIRE(doc.valid());
    REQUIRE(node.valid());
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
    REQUIRE_FALSE(threw);
    REQUIRE_FALSE(nodeValid);
    REQUIRE_FALSE(documentValid);
    REQUIRE(doc.valid());
    REQUIRE(node.valid());
    node.remove();
    REQUIRE_FALSE(node.valid());
    REQUIRE(doc.valid());
    auto moved = std::move(doc);
    REQUIRE_FALSE(doc.valid());
    REQUIRE(moved.valid());
    state->close();
    REQUIRE_FALSE(doc.valid());
    REQUIRE_FALSE(moved.valid());
    REQUIRE_FALSE(node.valid());
}

SCENARIO("DOM rejects empty event callbacks without changing subscriptions or pending names", "[dom]") {
    auto          state = std::make_shared<dom::detail::State>();
    dom::Document doc{Browser{state}};
    auto          button = doc.element("button");
    unsigned      calls{};
    button.listen("click", [&](std::string) {
        ++calls;
    });
    const auto size  = state->pending.data().size();
    const auto names = state->names.size();
    REQUIRE_THROWS_AS(button.listen("keydown", {}), std::invalid_argument);
    REQUIRE_THROWS_AS(button.listen("click", {}), std::invalid_argument);
    REQUIRE(state->pending.data().size() == size);
    REQUIRE(state->names.size() == names);
    REQUIRE(state->listeners.size() == 1);
    state->event(button.id(), state->names.at("click"), "");
    REQUIRE(calls == 1);
}

SCENARIO("An island unmount with pending writes closes the entire document once", "[dom]") {
    auto state   = std::make_shared<dom::detail::State>();
    state->ready = true;
    state->send  = [](const auto&) { };
    dom::Document doc{Browser{state}};
    auto          first = doc.island("first"), second = doc.island("second");
    auto          text = doc.text("second island");
    second.append(text);
    doc.flush();
    unsigned errors{};
    doc.onError([&](std::uint32_t, std::string reason) {
        REQUIRE(reason == "DOM island revoked with pending writes");
        ++errors;
    });
    text.setText("pending in a different island");
    state->revoke("first");
    REQUIRE(errors == 1);
    REQUIRE_FALSE(doc.valid());
    REQUIRE_FALSE(first.valid());
    REQUIRE_FALSE(second.valid());
    REQUIRE_FALSE(text.valid());
}


SCENARIO("Creating a grouped DOM read flushes writes even when its lazy read sender is discarded", "[dom]") {
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
    REQUIRE(writes == 0);
    static_cast<void>(doc.read({
        {text, dom::Read::text}
    }));
    REQUIRE(writes == 1);
    REQUIRE(*browser.starts == 0);
    auto sender = doc.read({
        {text, dom::Read::text}
    });
    REQUIRE(writes == 1);
    REQUIRE(*browser.starts == 0);
    REQUIRE(networking::ex::sync_wait(std::move(sender)).has_value());
    REQUIRE(*browser.starts == 1);
}

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
        state->send  = [](const auto&) { };
    }
};
}  // namespace

SCENARIO("An awaited DOM event subscribes when started and delivers one value", "[dom]") {
    ReadyDocument document;
    auto&         state = document.state;
    auto          input = document.doc.element("input");
    document.doc.island().append(input);
    document.doc.flush();
    GIVEN("a sender of the next input event") {
        auto sender = input.next("input", {.preventDefault = true, .stopPropagation = false});
        THEN("creating it subscribes nothing") {
            REQUIRE(pendingCodes(*state).empty());
            REQUIRE(state->waiters.empty());
        }
        WHEN("it is started") {
            Awaited awaited;
            auto    operation = ex::connect(sender, EventReceiver{&awaited, {}});
            ex::start(operation);
            const auto name = state->names.at("input");
            THEN("the browser subscription carries the flags declared by next()") {
                const auto operations = msg::DomBatch::decode(state->pending.data()).second;
                REQUIRE(operations.back().code == msg::DomOp::listen);
                REQUIRE(operations.back().a == input.id());
                REQUIRE(operations.back().b == name);
                REQUIRE(operations.back().c == 1);
                REQUIRE(awaited.completions == 0);
            }
            AND_WHEN("the event arrives twice") {
                document.doc.flush();
                state->event(input.id(), name, "first");
                state->event(input.id(), name, "second");
                THEN("only the first completes the wait, which unsubscribes") {
                    REQUIRE(awaited.completions == 1);
                    REQUIRE(awaited.value == "first");
                    REQUIRE(pendingCodes(*state) == std::vector{msg::DomOp::unlisten});
                    REQUIRE(state->waiters.empty());
                }
                AND_THEN("the same sender waits again for a later event") {
                    Awaited again;
                    auto    next = ex::connect(sender, EventReceiver{&again, {}});
                    ex::start(next);
                    state->event(input.id(), name, "third");
                    REQUIRE(again.value == "third");
                    REQUIRE(pendingCodes(*state) == std::vector{msg::DomOp::unlisten, msg::DomOp::listen, msg::DomOp::unlisten});
                }
            }
        }
    }
}

SCENARIO("An awaited DOM event completes stopped when it can no longer occur", "[dom]") {
    ReadyDocument document;
    auto&         state  = document.state;
    auto          island = document.doc.island("panel");
    auto          button = document.doc.element("button");
    island.append(button);
    document.doc.flush();
    Awaited awaited;
    auto    operation = ex::connect(button.next("click"), EventReceiver{&awaited, {}});
    ex::start(operation);
    REQUIRE(awaited.completions == 0);
    WHEN("the application unsubscribes the event") {
        button.unlisten("click");
        THEN("the wait stops") {
            REQUIRE(awaited.stopped);
        }
    }
    WHEN("the node is removed") {
        button.remove();
        THEN("the wait stops") {
            REQUIRE(awaited.stopped);
        }
    }
    WHEN("the page revokes the island") {
        state->revoke("panel");
        THEN("the wait stops") {
            REQUIRE(awaited.stopped);
            REQUIRE_FALSE(button.valid());
        }
    }
    WHEN("the browser disconnects") {
        state->close();
        THEN("the wait stops") {
            REQUIRE(awaited.stopped);
        }
    }
    WHEN("the browser rejects a batch") {
        bool reported{};
        document.doc.onError([&](std::uint32_t, std::string) {
            reported = awaited.stopped;
        });
        state->reject(1, "rejected");
        THEN("the wait stops before the error is reported") {
            REQUIRE(awaited.stopped);
            REQUIRE(reported);
        }
    }
    REQUIRE(awaited.completions == 1);
}

SCENARIO("An awaited DOM event is cancelled through its stop token", "[dom]") {
    ReadyDocument           document;
    auto&                   state  = document.state;
    auto                    button = document.doc.element("button");
    ex::inplace_stop_source source;
    Awaited                 awaited;
    GIVEN("a stop requested before start") {
        source.request_stop();
        auto operation = ex::connect(button.next("click"), EventReceiver{&awaited, source.get_token()});
        ex::start(operation);
        THEN("it completes stopped without subscribing") {
            REQUIRE(awaited.stopped);
            REQUIRE(awaited.completions == 1);
            REQUIRE(state->waiters.empty());
            REQUIRE_FALSE(state->names.contains("click"));
        }
    }
    GIVEN("a stop requested from another thread while waiting") {
        auto operation = ex::connect(button.next("click"), EventReceiver{&awaited, source.get_token()});
        ex::start(operation);
        std::thread([&] {
            source.request_stop();
        }).join();
        REQUIRE(awaited.stopped);
        const auto name = state->names.at("click");
        WHEN("the event arrives afterwards") {
            state->event(button.id(), name, "");
            THEN("the obsolete subscription is released without a second completion") {
                REQUIRE(awaited.completions == 1);
                REQUIRE(state->waiters.empty());
                REQUIRE(pendingCodes(*state).back() == msg::DomOp::unlisten);
            }
        }
        WHEN("a new wait or a callback subscribes the same event") {
            Awaited again;
            auto    next = ex::connect(button.next("click"), EventReceiver{&again, {}});
            ex::start(next);
            state->event(button.id(), name, "");
            REQUIRE(again.value == "");
            button.listen("click", [](std::string) { });
            THEN("the obsolete wait does not conflict") {
                REQUIRE(again.completions == 1);
                REQUIRE(state->listeners.size() == 1);
            }
        }
    }
}

SCENARIO("An awaited DOM event rejects conflicting subscriptions and invalid nodes", "[dom]") {
    ReadyDocument document;
    auto&         state  = document.state;
    auto          button = document.doc.element("button");
    auto          text   = document.doc.text("text");
    REQUIRE_THROWS_AS(text.next("click"), std::invalid_argument);
    REQUIRE_THROWS_AS(document.doc.island().next("click"), std::invalid_argument);
    Awaited first;
    auto    operation = ex::connect(button.next("click"), EventReceiver{&first, {}});
    ex::start(operation);
    const auto size = state->pending.data().size();
    WHEN("a second wait starts for the same event") {
        Awaited second;
        auto    other = ex::connect(button.next("click"), EventReceiver{&second, {}});
        ex::start(other);
        THEN("only the second fails, without changing the pending batch") {
            REQUIRE(second.error == "DOM event is already awaited");
            REQUIRE(first.completions == 0);
            REQUIRE(state->pending.data().size() == size);
        }
    }
    WHEN("a callback subscribes the awaited event") {
        REQUIRE_THROWS_AS(button.listen("click", [](std::string) { }), std::logic_error);
        THEN("the wait still receives it") {
            REQUIRE(state->pending.data().size() == size);
            state->event(button.id(), state->names.at("click"), "");
            REQUIRE(first.value == "");
        }
    }
    WHEN("a wait starts for an event with a callback") {
        button.listen("keydown", [](std::string) { });
        Awaited second;
        auto    other = ex::connect(button.next("keydown"), EventReceiver{&second, {}});
        ex::start(other);
        THEN("it fails") {
            REQUIRE(second.error == "DOM event already has a listener");
        }
    }
    WHEN("a wait starts on another thread") {
        Awaited second;
        auto    other = ex::connect(button.next("input"), EventReceiver{&second, {}});
        std::thread([&] {
            ex::start(other);
        }).join();
        THEN("it fails without touching the document") {
            REQUIRE(second.error == "DOM document requires the server thread");
            REQUIRE_FALSE(state->names.contains("input"));
        }
    }
    WHEN("the node is removed before a wait starts") {
        auto sender = button.next("input");
        button.remove();
        Awaited second;
        auto    other = ex::connect(sender, EventReceiver{&second, {}});
        ex::start(other);
        THEN("it fails") {
            REQUIRE(second.error == "DOM node is invalid");
        }
    }
}

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

SCENARIO("A coroutine is the state machine of a sequential DOM dialog", "[dom]") {
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
    ex::sync_wait(ex::when_all(wizard(next, back, steps), clicks(document.state, std::move(sequence))));
    REQUIRE(steps == std::vector<std::string>{"name: 1", "back: 2", "done: 3"});
    REQUIRE(document.state->waiters.empty());
}

namespace {
// Fills the pending batch until not even an unsubscription fits.
void fillBatch(dom::detail::State& state, std::uint32_t node) {
    try {
        for (;;)
            state.add({msg::DomOp::unlisten, node, 0});
    } catch (const std::length_error&) { }
}
}  // namespace

SCENARIO("A cancelled DOM event wait releases its browser subscription", "[dom]") {
    ReadyDocument           document;
    auto&                   state  = document.state;
    auto                    button = document.doc.element("button");
    ex::inplace_stop_source source;
    Awaited                 awaited;
    auto operation = ex::connect(button.next("click", {.preventDefault = true, .stopPropagation = true}), EventReceiver{&awaited, source.get_token()});
    ex::start(operation);
    const auto name = state->names.at("click");
    document.doc.flush();
    GIVEN("a stop requested on the server thread") {
        source.request_stop();
        THEN("the wait stops and its browser listener is removed at once") {
            REQUIRE(awaited.stopped);
            REQUIRE(state->waiters.empty());
            const auto operations = msg::DomBatch::decode(state->pending.data()).second;
            REQUIRE(operations.size() == 1);
            REQUIRE(operations[0].code == msg::DomOp::unlisten);
            REQUIRE(operations[0].a == button.id());
            REQUIRE(operations[0].b == name);
        }
        AND_WHEN("the event was already on its way") {
            state->event(button.id(), name, "");
            THEN("it is ignored") {
                REQUIRE(awaited.completions == 1);
                REQUIRE(pendingCodes(*state) == std::vector{msg::DomOp::unlisten});
            }
        }
    }
    GIVEN("stops requested from another thread") {
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
        THEN("one release is posted to the server context") {
            REQUIRE(awaited.stopped);
            REQUIRE(other.stopped);
            REQUIRE(posts == 1);
            REQUIRE(pendingCodes(*state).empty());
        }
        AND_WHEN("the server context runs it") {
            state->sweep();
            THEN("both browser listeners are removed") {
                REQUIRE(state->waiters.empty());
                REQUIRE(pendingCodes(*state) == std::vector{msg::DomOp::unlisten, msg::DomOp::unlisten});
            }
        }
        AND_WHEN("a new wait subscribes the same event before it runs") {
            Awaited again;
            auto    next = ex::connect(button.next("click"), EventReceiver{&again, {}});
            ex::start(next);
            state->sweep();
            THEN("the new subscription is kept") {
                REQUIRE(pendingCodes(*state) == std::vector{msg::DomOp::listen, msg::DomOp::unlisten});
                state->event(button.id(), name, "later");
                REQUIRE(again.value == "later");
            }
        }
    }
    GIVEN("a document closed before a stop from another thread") {
        unsigned posts{};
        state->post = [&] {
            ++posts;
        };
        state->close();
        THEN("the wait stopped with the document and nothing is posted") {
            REQUIRE(awaited.stopped);
            std::thread([&] {
                source.request_stop();
            }).join();
            REQUIRE(posts == 0);
            REQUIRE(awaited.completions == 1);
        }
    }
}

SCENARIO("A DOM unsubscription that does not fit a full batch opens the next one", "[dom]") {
    ReadyDocument           document;
    auto&                   state  = document.state;
    auto                    button = document.doc.element("button");
    ex::inplace_stop_source source;
    Awaited                 awaited;
    auto operation = ex::connect(button.next("click", {.preventDefault = true, .stopPropagation = false}), EventReceiver{&awaited, source.get_token()});
    ex::start(operation);
    const auto name = state->names.at("click");
    document.doc.flush();
    fillBatch(*state, button.id());
    WHEN("the awaited event arrives") {
        state->event(button.id(), name, "value");
        REQUIRE(awaited.value == "value");
    }
    WHEN("the wait is cancelled") {
        source.request_stop();
        REQUIRE(awaited.stopped);
    }
    REQUIRE(state->waiters.empty());
    document.doc.flush();
    const auto operations = msg::DomBatch::decode(state->pending.data()).second;
    REQUIRE(operations.size() == 1);
    REQUIRE(operations[0].code == msg::DomOp::unlisten);
    REQUIRE(operations[0].a == button.id());
    REQUIRE(operations[0].b == name);
}

SCENARIO("A DOM event wait that cannot be queued subscribes nothing", "[dom]") {
    ReadyDocument document;
    auto&         state  = document.state;
    auto          button = document.doc.element("button");
    state->intern("click");
    fillBatch(*state, button.id());
    Awaited awaited;
    auto    operation = ex::connect(button.next("click"), EventReceiver{&awaited, {}});
    ex::start(operation);
    THEN("it fails without a waiter") {
        REQUIRE_FALSE(awaited.error.empty());
        REQUIRE(state->waiters.empty());
    }
}
