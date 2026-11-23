// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The registry of the secondary windows and the session of one open(), shared by the native frontends (#652, #616).
#include <webfront/testing/Bdd.hpp>
#include <webfront/frontend/WindowRegistry.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace webfront::bdd;
using namespace webfront::frontend;

namespace {
std::vector<std::string> events;  // what the fake toolkit was asked, in order

// A window of a fake toolkit: it closes when asked, or by itself during its start.
struct FakeSession {
    std::string           page;
    bool                  closed = false;
    std::exception_ptr    failure;
    std::function<void()> closedHook;
    bool                  inCallback = false;  // true while its own closedHook runs
    bool                  closeOnStart = false;
    std::exception_ptr    failOnStart;

    explicit FakeSession(std::string name = {}) : page(std::move(name)) { }
    ~FakeSession() {
        events.push_back("destroy " + page);
        if (inCallback)
            events.push_back("destroyed in its own callback");
        if (closedHook)
            events.push_back("hook still set " + page);
    }
    void start() {
        events.push_back("start " + page);
        if (closeOnStart || failOnStart) {
            failure = failOnStart;
            close();
        }
    }
    void close() {
        closed = true;
        if (closedHook) {
            inCallback = true;
            closedHook();
            inCallback = false;
        }
    }
};

struct FakeHooks {
    static constexpr std::string_view name = "FakeFrontend";
    int                               reapsScheduled = 0;
    bool                              canClose       = true;

    template <typename Registry>
    void scheduleReap(Registry&) {
        ++reapsScheduled;
        events.push_back("schedule reap");
    }
    void cancelReap() {
        events.push_back("cancel reap");
    }
    void present(FakeSession& session) {
        events.push_back("present " + session.page);
    }
    bool close(FakeSession& session) {
        events.push_back("close " + session.page);
        if (!canClose)
            return false;
        session.close();
        return true;
    }
};

using Registry = detail::WindowRegistry<FakeSession, FakeHooks>;

void openPage(Registry& windows, const std::string& page, bool closeOnStart = false, std::exception_ptr failOnStart = {}) {
    windows.open(
        page,
        [&](FakeSession&) {
            auto session          = std::make_shared<FakeSession>(page);
            session->closeOnStart = closeOnStart;
            session->failOnStart  = failOnStart;
            events.push_back("create " + page);
            return session;
        },
        [](FakeSession& session, FakeSession&) {
            session.start();
        });
}

bool happened(const std::string& event) {
    return std::ranges::find(events, event) != events.end();
}

struct RegistryTag;
struct SessionTag;
struct PublishTag;
}  // namespace

const Scenario secondaryWindowRegistry{"The registry of the secondary windows is shared by the native frontends", {"window-registry"}, [] {
    given("a registry whose first window is open", [&] {
        events.clear();
        Registry     windows;
        FakeSession  first("first");
        windows.primary = &first;

        when("a page is opened", [&] {
            openPage(windows, "a.html");
            then("the window is created, started and registered", [&] {
                require(windows.secondary.size() == 1);
                check(events == std::vector<std::string>{"create a.html", "start a.html"});
            });
            andWhen("the same page is opened again", [&] {
                openPage(windows, "a.html");
                then("the window already showing it comes to the front and nothing is created", [&] {
                    check(happened("present a.html"));
                    check(std::ranges::count(events, "create a.html") == 1);
                    check(windows.secondary.size() == 1);
                });
            });
            andWhen("its window closes", [&] {
                require(windows.close("a.html"));
                then("it leaves the table, is destroyed later and not in its own callback", [&] {
                    check(windows.secondary.empty());
                    check(windows.finished.size() == 1);
                    check(windows.hooks.reapsScheduled == 1);
                    check(!happened("destroy a.html"));
                    windows.reap();
                    check(happened("destroy a.html"));
                    check(!happened("destroyed in its own callback"));
                    check(windows.finished.empty());
                });
                andWhen("the page is opened again", [&] {
                    windows.reap();
                    openPage(windows, "a.html");
                    then("it is a new window", [&] {
                        check(std::ranges::count(events, "create a.html") == 2);
                        check(windows.secondary.size() == 1);
                    });
                });
            });
            andWhen("closing is asked for a page nobody shows, or for one that cannot close", [&] {
                windows.hooks.canClose = false;
                then("it answers false", [&] {
                    check(!windows.close("b.html"));
                    check(!windows.close("a.html"));
                });
            });
        });

        when("a window closes by itself while it starts", [&] {
            then("the caller learns why, and the page is not registered", [&] {
                checkThrows<std::runtime_error>([&] {
                    openPage(windows, "a.html", true, std::make_exception_ptr(std::runtime_error("navigation failed")));
                });
                check(windows.secondary.empty());
            });
            andWhen("it closed without a failure", [&] {
                then("the caller still gets an error that names the page", [&] {
                    checkThrows<std::runtime_error>([&] { openPage(windows, "b.html", true); }, contains("'b.html' closed while it started"));
                    check(windows.secondary.empty());
                });
            });
            andWhen("the page is opened once more", [&] {
                windows.reap();
                openPage(windows, "a.html");
                then("it works", [&] {
                    check(windows.secondary.size() == 1);
                });
            });
        });

        when("the first window closes in the pass that handles the request", [&] {
            first.closed = true;
            then("nothing is built and the caller is told", [&] {
                checkThrows<std::logic_error>([&] { openPage(windows, "a.html"); }, contains("FakeFrontend: no window of the application is open"));
                check(!happened("create a.html"));
            });
        });

        when("the first window ends with secondary windows open", [&] {
            openPage(windows, "a.html");
            openPage(windows, "b.html");
            require(windows.close("a.html"));
            events.clear();
            windows.clear();
            then("the reap is withdrawn and every session is destroyed with its hook cleared", [&] {
                check(events.front() == "cancel reap");
                check(happened("destroy a.html"));
                check(happened("destroy b.html"));
                check(!happened("hook still set b.html"));
                check(windows.secondary.empty() && windows.finished.empty());
                check(windows.primary == nullptr);
            });
        });
        windows.clear();
    });
    given("a registry without a first window", [&] {
        Registry windows;
        then("opening is refused", [&] {
            checkThrows<std::logic_error>([&] { openPage(windows, "a.html"); }, contains("show() it before opening another"));
        });
    });
}};

const Scenario interfaceSessionOrder{"The session of an open() attaches, publishes and withdraws in order", {"window-registry"}, [] {
    given("an open() that attaches its loop", [&] {
        auto& host       = uiExecutorHostOf<SessionTag>();
        auto& windowHost = nativeWindowHostOf<SessionTag>();
        int   handle     = 0;
        Registry    windows;
        FakeSession first("first");
        const auto  published = [&] {
            return windowHost.window();
        };
        when("the session is held", [&] {
            {
                detail::InterfaceSession<Registry> session(host, windowHost, windows, first, [] {}, {.kind = NativeWindowKind::gtk, .pointer = &handle, .portalParent = {}});
                then("the executor is available, the window published and the first window registered", [&] {
                    check(host.executor().available());
                    check(windows.primary == &first);
                });
                andWhen("another open() starts", [&] {
                    then("it is refused", [&] {
                        checkThrows<std::logic_error>([&] { detail::InterfaceSession<Registry>::refuseSecond(host, "FakeFrontend"); },
                                                      contains("FakeFrontend: open() is already running"));
                    });
                });
                openPage(windows, "a.html");
                events.clear();
            }
            then("the secondary windows are cleared first, then the window is withdrawn and the host closed", [&] {
                check(!events.empty() && events.front() == "cancel reap");
                check(windows.primary == nullptr && windows.secondary.empty());
                check(!host.executor().available());
                check(!published().get().has_value());
            });
            andWhen("a later open() starts", [&] {
                then("it is accepted", [&] {
                    checkNothrow([&] { detail::InterfaceSession<Registry>::refuseSecond(host, "FakeFrontend"); });
                });
            });
        });
    });
    given("a loop that is already attached", [&] {
        auto& host = uiExecutorHostOf<PublishTag>();
        host.attach([] {});
        Registry    windows;
        FakeSession first("first");
        int         handle = 0;
        then("a second session throws and leaves the first loop attached", [&] {
            checkThrows<std::logic_error>([&] {
                detail::InterfaceSession<Registry> session(host, nativeWindowHostOf<PublishTag>(), windows, first, [] {}, {.kind = NativeWindowKind::gtk, .pointer = &handle, .portalParent = {}});
            });
            check(host.executor().available());
            check(windows.primary == nullptr);
        });
        host.close();
    });
}};
