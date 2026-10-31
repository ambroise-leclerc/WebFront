#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/components/react/Store.hpp>
#include <webfront/tooling/Logger.hpp>
#include <webfront/WebFront.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
struct Harness {
    std::shared_ptr<webfront::dom::detail::State>                                    document = std::make_shared<webfront::dom::detail::State>();
    std::vector<int>                                                                 sent;
    std::vector<webfront::testing::ManualSender::Completion>                         timers;
    bool                                                                             fail{};
    webfront::BasicWF<webfront::networking::NetworkingMock, webfront::fs::DefaultFS> app{webfront::WebFrontConfig{.port = "0"}};
    void                                                                             tick() {
        auto completion = std::move(timers.back());
        timers.pop_back();
        completion.complete({}, 0);
    }
};
struct UI {
    Harness* harness;
    auto     domState() const {
        return harness->document;
    }
    auto document() const {
        return webfront::dom::Document{*this};
    }
    auto resumeAfter(std::chrono::milliseconds delay) const {
        REQUIRE(delay == std::chrono::milliseconds(1));
        return webfront::testing::ManualSender([h = harness](auto completion) {
            h->timers.push_back(std::move(completion));
        });
    }
    auto spawn(webfront::Task task) const {
        return harness->app.spawn(std::move(task));
    }
    auto jsFunction(std::string_view name) const {
        REQUIRE(name == "webfrontReactStores.counter");
        return [h = harness](int value) {
            if (h->fail)
                throw std::runtime_error("send failed");
            h->sent.push_back(value);
        };
    }
};
}  // namespace

SCENARIO("React stores coalesce a thousand writes into one snapshot per server cycle", "[react]") {
    Harness                         harness;
    webfront::react::Store<UI, int> store{UI{&harness}, "counter"};
    auto                            copy = store;
    for (int i = 0; i < 1000; ++i)
        copy.set(i);
    REQUIRE(harness.sent.empty());
    REQUIRE(harness.timers.size() == 1);
    harness.tick();
    REQUIRE(harness.sent == std::vector<int>{999});
    WHEN("an explicit flush precedes the timer") {
        store.set(1000);
        store.flush();
        harness.tick();
        REQUIRE(harness.sent == std::vector<int>{999, 1000});
        store.set(1001);
        harness.tick();
        REQUIRE(harness.sent.back() == 1001);
    }
    WHEN("the connection closes before the next cycle") {
        store.set(1000);
        harness.document->close();
        harness.tick();
        REQUIRE(harness.sent.size() == 1);
        REQUIRE_FALSE(store.valid());
        REQUIRE_THROWS(store.set(1001));
    }
    WHEN("the application shuts down with a pending cycle") {
        store.set(1000);
        harness.app.shutdown();
        REQUIRE(harness.sent.size() == 1);
    }
    WHEN("the store closes explicitly") {
        store.set(1000);
        store.close();
        harness.tick();
        REQUIRE_FALSE(copy.valid());
        REQUIRE(harness.sent.size() == 1);
        REQUIRE_THROWS(copy.flush());
    }
    WHEN("close releases a self-capturing error handler") {
        auto               lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> observed = lifetime;
        store.onError([keep = copy, lifetime](std::exception_ptr) {
            CHECK_FALSE(keep.valid());
            CHECK(*lifetime == 7);
        });
        lifetime.reset();
        REQUIRE_FALSE(observed.expired());
        store.close();
        REQUIRE(observed.expired());
    }
    WHEN("automatic failure releases a self-capturing error handler") {
        auto               lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> observed = lifetime;
        unsigned           calls{};
        store.onError([keep = copy, lifetime, &calls](std::exception_ptr) {
            ++calls;
            CHECK_FALSE(keep.valid());
            CHECK(*lifetime == 7);
        });
        lifetime.reset();
        harness.fail = true;
        store.set(1000);
        harness.tick();
        REQUIRE(calls == 1);
        REQUIRE(observed.expired());
    }
    WHEN("sending the automatic snapshot fails") {
        unsigned errors{};
        store.onError([&](std::exception_ptr error) {
            ++errors;
            REQUIRE_THROWS(std::rethrow_exception(error));
        });
        harness.fail = true;
        store.set(1000);
        harness.tick();
        REQUIRE(errors == 1);
        REQUIRE_FALSE(store.valid());
    }
    WHEN("an explicit flush fails before the timer fires") {
        unsigned errors{};
        store.onError([&](std::exception_ptr) {
            ++errors;
        });
        store.set(1000);
        harness.fail = true;
        REQUIRE_THROWS_AS(store.flush(), std::runtime_error);
        REQUIRE(store.valid());
        REQUIRE(errors == 0);
        harness.fail = false;
        store.flush();
        harness.tick();
        REQUIRE(harness.sent == std::vector<int>{999, 1000});
    }
    WHEN("the automatic error callback throws") {
        std::vector<std::string> logs;
        const auto               enabled = webfront::log::is(webfront::log::Error);
        webfront::log::set(webfront::log::Error, true);
        auto               sink     = webfront::log::addSinks([&](std::string_view text) {
            logs.emplace_back(text);
        });
        auto               lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> observed = lifetime;
        store.onError([keep = copy, lifetime](std::exception_ptr) {
            CHECK_FALSE(keep.valid());
            CHECK(*lifetime == 7);
            throw std::runtime_error("handler failed");
        });
        lifetime.reset();
        harness.fail = true;
        store.set(1000);
        harness.tick();
        webfront::log::removeSinks(sink);
        webfront::log::set(webfront::log::Error, enabled);
        REQUIRE_FALSE(store.valid());
        REQUIRE(logs.size() == 1);
        REQUIRE(logs.front().find("React store onError callback threw") != std::string::npos);
        REQUIRE(observed.expired());
    }
    WHEN("a wrapper is moved while a snapshot is pending") {
        store.set(1000);
        auto moved = std::move(copy);
        REQUIRE_FALSE(copy.valid());
        REQUIRE_THROWS_AS(copy.set(1001), std::runtime_error);
        REQUIRE_THROWS_AS(copy.flush(), std::runtime_error);
        REQUIRE_THROWS_AS(copy.close(), std::runtime_error);
        REQUIRE_THROWS_AS(copy.onError({}), std::runtime_error);
        REQUIRE(moved.valid());
        REQUIRE(store.valid());
        harness.tick();
        REQUIRE(harness.sent == std::vector<int>{999, 1000});
        moved.set(1002);
        moved.flush();
        harness.tick();
        REQUIRE(harness.sent == std::vector<int>{999, 1000, 1002});
    }
    WHEN("another thread attempts to update it") {
        bool        rejected{};
        std::thread other([&] {
            try {
                store.set(1000);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
        });
        other.join();
        REQUIRE(rejected);
        REQUIRE(harness.timers.empty());
    }
}
