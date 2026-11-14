#include "InjectableNetworking.hpp"
#include "Bdd.hpp"
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

using namespace webfront::bdd;

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
        require(delay == std::chrono::milliseconds(1));
        return webfront::testing::ManualSender([h = harness](auto completion) {
            h->timers.push_back(std::move(completion));
        });
    }
    auto spawn(webfront::Task task) const {
        return harness->app.spawn(std::move(task));
    }
    auto jsFunction(std::string_view name) const {
        require(name == "webfrontReactStores.counter");
        return [h = harness](int value) {
            if (h->fail)
                throw std::runtime_error("send failed");
            h->sent.push_back(value);
        };
    }
};
}  // namespace

Scenario reactStores{"React stores coalesce a thousand writes into one snapshot per server cycle", {"react"}, [] {
    Harness                         harness;
    webfront::react::Store<UI, int> store{UI{&harness}, "counter"};
    auto                            copy = store;
    for (int i = 0; i < 1000; ++i)
        copy.set(i);
    require(harness.sent.empty());
    require(harness.timers.size() == 1);
    harness.tick();
    require(harness.sent == std::vector<int>{999});
    when("an explicit flush precedes the timer", [&] {
        store.set(1000);
        store.flush();
        harness.tick();
        require(harness.sent == std::vector<int>{999, 1000});
        store.set(1001);
        harness.tick();
        require(harness.sent.back() == 1001);
    });
    when("the connection closes before the next cycle", [&] {
        store.set(1000);
        harness.document->close();
        harness.tick();
        require(harness.sent.size() == 1);
        require(!store.valid());
        requireThrows([&] { store.set(1001); });
    });
    when("the application shuts down with a pending cycle", [&] {
        store.set(1000);
        harness.app.shutdown();
        require(harness.sent.size() == 1);
    });
    when("the store closes explicitly", [&] {
        store.set(1000);
        store.close();
        harness.tick();
        require(!copy.valid());
        require(harness.sent.size() == 1);
        requireThrows([&] { copy.flush(); });
    });
    when("close releases a self-capturing error handler", [&] {
        auto               lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> observed = lifetime;
        store.onError([keep = copy, lifetime](std::exception_ptr) {
            check(!keep.valid());
            check(*lifetime == 7);
        });
        lifetime.reset();
        require(!observed.expired());
        store.close();
        require(observed.expired());
    });
    when("automatic failure releases a self-capturing error handler", [&] {
        auto               lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> observed = lifetime;
        unsigned           calls{};
        store.onError([keep = copy, lifetime, &calls](std::exception_ptr) {
            ++calls;
            check(!keep.valid());
            check(*lifetime == 7);
        });
        lifetime.reset();
        harness.fail = true;
        store.set(1000);
        harness.tick();
        require(calls == 1);
        require(observed.expired());
    });
    when("sending the automatic snapshot fails", [&] {
        unsigned errors{};
        store.onError([&](std::exception_ptr error) {
            ++errors;
            require(error != nullptr);
        });
        harness.fail = true;
        store.set(1000);
        harness.tick();
        require(errors == 1);
        require(!store.valid());
    });
    when("an explicit flush fails before the timer fires", [&] {
        unsigned errors{};
        store.onError([&](std::exception_ptr) {
            ++errors;
        });
        store.set(1000);
        harness.fail = true;
        requireThrows<std::runtime_error>([&] { store.flush(); });
        require(store.valid());
        require(errors == 0);
        harness.fail = false;
        store.flush();
        harness.tick();
        require(harness.sent == std::vector<int>{999, 1000});
    });
    when("the automatic error callback throws", [&] {
        std::vector<std::string> logs;
        const auto               enabled = webfront::log::is(webfront::log::Error);
        webfront::log::set(webfront::log::Error, true);
        auto               sink     = webfront::log::addSinks([&](std::string_view text) {
            logs.emplace_back(text);
        });
        auto               lifetime = std::make_shared<int>(7);
        std::weak_ptr<int> observed = lifetime;
        store.onError([keep = copy, lifetime](std::exception_ptr) {
            check(!keep.valid());
            check(*lifetime == 7);
            throw std::runtime_error("handler failed");
        });
        lifetime.reset();
        harness.fail = true;
        store.set(1000);
        harness.tick();
        webfront::log::removeSinks(sink);
        webfront::log::set(webfront::log::Error, enabled);
        require(!store.valid());
        require(logs.size() == 1);
        require(logs.front().find("React store onError callback threw") != std::string::npos);
        require(observed.expired());
    });
    when("a wrapper is moved while a snapshot is pending", [&] {
        store.set(1000);
        auto moved = std::move(copy);
        require(!copy.valid());
        requireThrows<std::runtime_error>([&] { copy.set(1001); });
        requireThrows<std::runtime_error>([&] { copy.flush(); });
        requireThrows<std::runtime_error>([&] { copy.close(); });
        requireThrows<std::runtime_error>([&] { copy.onError({}); });
        require(moved.valid());
        require(store.valid());
        harness.tick();
        require(harness.sent == std::vector<int>{999, 1000});
        moved.set(1002);
        moved.flush();
        harness.tick();
        require(harness.sent == std::vector<int>{999, 1000, 1002});
    });
    when("another thread attempts to update it", [&] {
        bool        rejected{};
        std::thread other([&] {
            try {
                store.set(1000);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
        });
        other.join();
        require(rejected);
        require(harness.timers.empty());
    });
}};
