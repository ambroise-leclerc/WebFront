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
