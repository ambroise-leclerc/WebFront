#include <webfront/testing/Bdd.hpp>
#include <webfront/UISubscribers.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

namespace {

struct FakeUI {
    int id;
};

using Subscribers = detail::UISubscribers<FakeUI>;

}  // namespace

const Scenario severalHandlers{"Several handlers subscribe to connected browsers", [] {
                                   given("two subscriptions", [&] {
                                       auto           subscribers = make_shared<Subscribers>();
                                       vector<string> calls;
                                       auto           first  = subscribers->subscribe([&calls](FakeUI ui) {
                                           calls.push_back("first " + to_string(ui.id));
                                       });
                                       auto           second = subscribers->subscribe([&calls](FakeUI ui) {
                                           calls.push_back("second " + to_string(ui.id));
                                       });

                                       when("a browser connects", [&] {
                                           subscribers->notify(FakeUI{7});
                                           then("every handler receives it, in subscription order", [&] {
                                               require(calls == vector<string>{"first 7", "second 7"});
                                           });
                                       });
                                       when("the first subscription is reset", [&] {
                                           first.reset();
                                           subscribers->notify(FakeUI{8});
                                           then("only the remaining handler is called", [&] {
                                               require(!first.active());
                                               require(calls == vector<string>{"second 8"});
                                           });
                                       });
                                       when("a subscription is destroyed", [&] {
                                           optional<UISubscription> scoped{subscribers->subscribe([&calls](FakeUI) {
                                               calls.push_back("scoped");
                                           })};
                                           scoped.reset();
                                           subscribers->notify(FakeUI{9});
                                           then("its handler is no longer called", [&] {
                                               require(calls == vector<string>{"first 9", "second 9"});
                                           });
                                       });
                                   });
                               }};

const Scenario subscriptionsMove{"Subscriptions are move-only handles", [] {
                                     given("a subscription moved into another one", [&] {
                                         auto           subscribers = make_shared<Subscribers>();
                                         int            calls       = 0;
                                         auto           original    = subscribers->subscribe([&calls](FakeUI) {
                                             ++calls;
                                         });
                                         UISubscription moved{std::move(original)};

                                         then("the handler stays subscribed and only the destination owns it", [&] {
                                             subscribers->notify(FakeUI{1});
                                             require(calls == 1);
                                             require(!original.active());  // NOLINT(bugprone-use-after-move): moved-from state is specified
                                             require(moved.active());
                                         });
                                         when("a subscription is assigned over an active one", [&] {
                                             moved = subscribers->subscribe([](FakeUI) {});
                                             then("the overwritten handler is unsubscribed", [&] {
                                                 require(subscribers->size() == 1);
                                             });
                                         });
                                     });
                                 }};

const Scenario subscriptionMay{"A subscription may outlive its application", [] {
                                   given("a subscription whose subscribers were destroyed", [&] {
                                       auto subscribers  = make_shared<Subscribers>();
                                       auto subscription = subscribers->subscribe([](FakeUI) {});
                                       subscribers.reset();

                                       then("resetting it is harmless", [&] {
                                           requireNothrow([&] {
                                               subscription.reset();
                                           });
                                           require(!subscription.active());
                                       });
                                   });
                               }};

const Scenario handlersMay{"Handlers may change the subscriptions while being notified", [] {
                               given("a handler that subscribes another handler and drops its own subscription", [&] {
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

                                   when("two browsers connect", [&] {
                                       subscribers->notify(FakeUI{1});
                                       subscribers->notify(FakeUI{2});
                                       then("the changes apply from the next browser, without deadlock", [&] {
                                           require(calls == vector<string>{"self", "added"});
                                       });
                                   });
                               });
                           }};

const Scenario handlerMayOwn{"A handler may own another subscription to the same browsers", [] {
                                 given("a handler whose capture owns a second subscription", [&] {
                                     auto subscribers = make_shared<Subscribers>();
                                     auto inner       = make_shared<UISubscription>(subscribers->subscribe([](FakeUI) {}));
                                     auto outer       = subscribers->subscribe([inner](FakeUI) {});
                                     inner.reset();  // the outer handler's capture is now the only owner of the inner subscription

                                     when("the outer subscription is reset", [&] {
                                         outer.reset();  // destroying the outer handler unsubscribes the inner one
                                         then("both are unsubscribed, without deadlock", [&] {
                                             require(subscribers->size() == 0);
                                         });
                                     });
                                 });
                             }};
