#include <catch2/catch_test_macros.hpp>
#include <UISubscribers.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace std;
using namespace webfront;

namespace {

struct FakeUI {
    int id;
};

using Subscribers = detail::UIStartedSubscribers<FakeUI>;

}  // namespace

SCENARIO("Several handlers subscribe to connected browsers") {
    GIVEN("two subscriptions") {
        auto           subscribers = make_shared<Subscribers>();
        vector<string> calls;
        auto           first  = subscribers->subscribe([&calls](FakeUI ui) {
            calls.push_back("first " + to_string(ui.id));
        });
        auto           second = subscribers->subscribe([&calls](FakeUI ui) {
            calls.push_back("second " + to_string(ui.id));
        });

        WHEN("a browser connects") {
            subscribers->notify(FakeUI{7});
            THEN("every handler receives it, in subscription order") {
                REQUIRE(calls == vector<string>{"first 7", "second 7"});
            }
        }
        WHEN("the first subscription is reset") {
            first.reset();
            subscribers->notify(FakeUI{8});
            THEN("only the remaining handler is called") {
                REQUIRE_FALSE(first.active());
                REQUIRE(calls == vector<string>{"second 8"});
            }
        }
        WHEN("a subscription is destroyed") {
            optional<UISubscription> scoped{subscribers->subscribe([&calls](FakeUI) {
                calls.push_back("scoped");
            })};
            scoped.reset();
            subscribers->notify(FakeUI{9});
            THEN("its handler is no longer called") {
                REQUIRE(calls == vector<string>{"first 9", "second 9"});
            }
        }
    }
}

SCENARIO("Subscriptions are move-only handles") {
    GIVEN("a subscription moved into another one") {
        auto           subscribers = make_shared<Subscribers>();
        int            calls       = 0;
        auto           original    = subscribers->subscribe([&calls](FakeUI) {
            ++calls;
        });
        UISubscription moved{std::move(original)};

        THEN("the handler stays subscribed and only the destination owns it") {
            subscribers->notify(FakeUI{1});
            REQUIRE(calls == 1);
            REQUIRE_FALSE(original.active());  // NOLINT(bugprone-use-after-move): moved-from state is specified
            REQUIRE(moved.active());
        }
        WHEN("a subscription is assigned over an active one") {
            moved = subscribers->subscribe([](FakeUI) {});
            THEN("the overwritten handler is unsubscribed") {
                REQUIRE(subscribers->size() == 1);
            }
        }
    }
}

SCENARIO("A subscription may outlive its application") {
    GIVEN("a subscription whose subscribers were destroyed") {
        auto subscribers  = make_shared<Subscribers>();
        auto subscription = subscribers->subscribe([](FakeUI) {});
        subscribers.reset();

        THEN("resetting it is harmless") {
            REQUIRE_NOTHROW(subscription.reset());
            REQUIRE_FALSE(subscription.active());
        }
    }
}

SCENARIO("Handlers may change the subscriptions while being notified") {
    GIVEN("a handler that subscribes another handler and drops its own subscription") {
        auto           subscribers = make_shared<Subscribers>();
        vector<string> calls;
        UISubscription self;
        UISubscription added;
        self = subscribers->subscribe([&](FakeUI) {
            calls.push_back("self");
            added = subscribers->subscribe([&calls](FakeUI) {
                calls.push_back("added");
            });
            self.reset();
        });

        WHEN("two browsers connect") {
            subscribers->notify(FakeUI{1});
            subscribers->notify(FakeUI{2});
            THEN("the changes apply from the next browser, without deadlock") {
                REQUIRE(calls == vector<string>{"self", "added"});
            }
        }
    }
}
