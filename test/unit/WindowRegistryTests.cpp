// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The registry of the secondary windows and the session of one open(), shared by the native frontends (#652, #616).
#include <webfront/frontend/EmbeddedFrontend.hpp>
#include <webfront/frontend/Frontend.hpp>
#include <webfront/frontend/StartupTrace.hpp>
#include <webfront/frontend/WindowRegistry.hpp>
#include <webfront/testing/Bdd.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace webfront::bdd;
using namespace webfront;
using namespace webfront::frontend;

namespace {
std::vector<std::string> events;  // what the fake toolkit was asked, in order

// A window of a fake toolkit: it closes when asked, or by itself during its start.
struct FakeSession {
    std::string                         page;
    bool                                closed = false;
    std::exception_ptr                  failure;
    std::function<void()>               closedHook;
    bool                                inCallback   = false;  // true while its own closedHook runs
    bool                                closeOnStart = false;
    std::exception_ptr                  failOnStart;
    static inline std::function<void()> destroyProbe;  // runs in every destructor: what a window's teardown can observe

    explicit FakeSession(std::string name = {}) : page(std::move(name)) {}
    ~FakeSession() {
        events.push_back("destroy " + page);
        if (destroyProbe)
            destroyProbe();
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
    static constexpr std::string_view name           = "FakeFrontend";
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

using Registry = frontend::detail::WindowRegistry<FakeSession, FakeHooks>;

bool openPage(Registry& windows, const std::string& page, bool closeOnStart = false, std::exception_ptr failOnStart = {}) {
    return windows.open(
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

const Scenario secondaryWindowRegistry{
    "The registry of the secondary windows is shared by the native frontends",
    {"window-registry"},
    [] {
        given("a registry whose first window is open", [&] {
            events.clear();
            Registry    windows;
            FakeSession first("first");
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
                    checkThrows<std::runtime_error>(
                        [&] {
                            openPage(windows, "a.html", true, std::make_exception_ptr(std::runtime_error("navigation failed")));
                        },
                        contains("navigation failed"));
                    check(windows.secondary.empty());
                });
                andWhen("it closed without a failure", [&] {
                    then("the caller still gets an error that names the page", [&] {
                        checkThrows<std::runtime_error>(
                            [&] {
                                openPage(windows, "b.html", true);
                            },
                            contains("'b.html' closed while it started"));
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
                    checkThrows<std::logic_error>(
                        [&] {
                            openPage(windows, "a.html");
                        },
                        contains("FakeFrontend: no window of the application is open"));
                    check(!happened("create a.html"));
                });
            });

            when("the first window closes while a secondary one still shows the page", [&] {
                openPage(windows, "a.html");
                events.clear();
                first.closed = true;
                then("asking for that page brings its window to the front instead of failing", [&] {
                    openPage(windows, "a.html");
                    check(!happened("create a.html"));
                    check(windows.secondary.size() == 1);
                });
            });

            when("a window opens a page while the others are destroyed", [&] {
                openPage(windows, "a.html");
                bool lateOpened           = true;
                FakeSession::destroyProbe = [&] {
                    lateOpened = openPage(windows, "late.html");  // a destructor cannot be refused by an exception: it is told false
                };
                windows.clear();
                FakeSession::destroyProbe = nullptr;
                then("no window outlives the session, and the request is told it was refused", [&] {
                    check(!lateOpened);
                    check(windows.secondary.empty());
                    check(!happened("create late.html"));
                });
            });

            when("the first window ends with secondary windows open", [&] {
                openPage(windows, "a.html");
                openPage(windows, "b.html");
                require(windows.close("a.html"));
                events.clear();
                windows.clear();
                then("the reap is withdrawn and every session is destroyed with its hook cleared", [&] {
                    require(!events.empty());
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
                checkThrows<std::logic_error>(
                    [&] {
                        openPage(windows, "a.html");
                    },
                    contains("show() it before opening another"));
            });
        });
    }};

const Scenario interfaceSessionOrder{
    "The session of an open() attaches, publishes and withdraws in order",
    {"window-registry"},
    [] {
        given("an open() that attaches its loop", [&] {
            auto&       host       = uiExecutorHostOf<SessionTag>();
            auto&       windowHost = nativeWindowHostOf<SessionTag>();
            int         handle     = 0;
            Registry    windows;
            FakeSession first("first");
            const auto  published = [&] {
                return windowHost.window();
            };
            when("the session is held", [&] {
                {
                    frontend::detail::InterfaceSession<Registry> session(host,
                                                                         windowHost,
                                                                         windows,
                                                                         first,
                                                                         [] {},
                                                                         {.kind = NativeWindowKind::gtk, .pointer = &handle, .portalParent = {}});
                    then("the executor is available, the window published and the first window registered", [&] {
                        check(host.executor().available());
                        check(windows.primary == &first);
                        const auto window = published().get();
                        require(window.has_value());
                        check(window->pointer == &handle);
                    });
                    andWhen("another open() starts", [&] {
                        then("it is refused", [&] {
                            checkThrows<std::logic_error>(
                                [&] {
                                    frontend::detail::InterfaceSession<Registry>::refuseSecond(host, "FakeFrontend");
                                },
                                contains("FakeFrontend: open() is already running"));
                        });
                    });
                    openPage(windows, "a.html");
                    events.clear();
                    FakeSession::destroyProbe = [&] {  // the secondary windows are destroyed while the window is still published
                        if (!published().get().has_value() || !host.executor().available())
                            events.push_back("withdrawn too early");
                    };
                }
                FakeSession::destroyProbe = nullptr;
                then("the secondary windows are cleared first, then the window is withdrawn and the host closed", [&] {
                    check(!events.empty() && events.front() == "cancel reap");
                    check(happened("destroy a.html") && !happened("withdrawn too early"));
                    check(windows.primary == nullptr && windows.secondary.empty());
                    check(!host.executor().available());
                    check(!published().get().has_value());
                });
                andWhen("a later open() starts", [&] {
                    then("it is accepted", [&] {
                        checkNothrow([&] {
                            frontend::detail::InterfaceSession<Registry>::refuseSecond(host, "FakeFrontend");
                        });
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
                    frontend::detail::InterfaceSession<Registry> session(host,
                                                                         nativeWindowHostOf<PublishTag>(),
                                                                         windows,
                                                                         first,
                                                                         [] {},
                                                                         {.kind = NativeWindowKind::gtk, .pointer = &handle, .portalParent = {}});
                });
                check(host.executor().available());
                check(windows.primary == nullptr);
            });
            host.close();
        });
    }};

namespace window_registry_fakes {  // named, not anonymous: the clang warning for unused constants of anonymous types
struct FakeHost;
struct UnavailableHost;

// A native frontend as a toolkit declares it: the facade supplies the rest.
struct FakeFrontend : EmbeddedFrontend<FakeFrontend> {
    static constexpr bool             requiresMainThread = false;
    static constexpr bool             secondaryWindows   = true;
    static constexpr bool             compiled           = true;
    static constexpr std::string_view name               = "FakeFrontend";
    static constexpr std::string_view enable             = "Configure it.";
    using Host                                           = FakeHost;
    using Options                                        = ViewOptions;
    static inline std::vector<std::string> opened;
    static void                            openNative(std::string_view address, std::string_view port, std::string_view file, const Options& options) {
        opened.push_back(std::string(address) + ":" + std::string(port) + "/" + std::string(file)
                         + (options.window.title.empty() ? "" : " " + options.window.title) + (options.launch.secret.empty() ? "" : " launched")
                         + (options.contentPolicy.devToolsEnabled ? "" : " nodevtools"));
    }
    static void                            closeNative(const NativeWindowHandle&) {}
    static inline std::vector<std::string> openedSecondary;
    static void                            openSecondaryNative(std::string_view, std::string_view, std::string_view page, const Options& options) {
        openedSecondary.push_back(std::string(page) + (options.contentPolicy.devToolsEnabled ? "" : " nodevtools") + " " + options.window.title + " "
                                  + options.launch.secret);
    }
    static bool closeSecondaryNative(std::string_view) {
        return true;
    }
};

// A frontend whose options come from a provider (WebView2FrontendWithOptions): a call overrides only what it passes.
struct ConfiguredFrontend : EmbeddedFrontend<ConfiguredFrontend> {
    static constexpr bool             secondaryWindows = false;
    static constexpr bool             compiled         = true;
    static constexpr std::string_view name             = "ConfiguredFrontend";
    using Host                                         = ConfiguredFrontend;
    using Options                                      = ViewOptions;
    static inline std::vector<std::string> opened;
    static ViewOptions                     defaultOptions() {
        return {
                                .window = {.title = "configured"},
                                .launch = {"configured-secret", "configured-origin"}
        };
    }
    static void openNative(std::string_view, std::string_view, std::string_view file, const Options& options) {
        opened.push_back(std::string(file) + " " + options.window.title + " " + options.launch.secret);
    }
    static void closeNative(const NativeWindowHandle&) {}
};

// A configured frontend with secondary windows (WebView2FrontendWithOptions): a secondary window does not ask the provider.
struct ConfiguredSecondaryFrontend : EmbeddedFrontend<ConfiguredSecondaryFrontend> {
    static constexpr bool             secondaryWindows   = true;
    static constexpr bool             compiled           = true;
    static constexpr std::string_view name               = "ConfiguredSecondaryFrontend";
    using Host                                           = ConfiguredSecondaryFrontend;
    using Options                                        = ViewOptions;
    static inline int                      providerCalls = 0;
    static inline std::vector<std::string> openedSecondary;
    static ViewOptions                     defaultOptions() {
        ++providerCalls;
        throw std::runtime_error("the provider failed");
    }
    static void openNative(std::string_view, std::string_view, std::string_view, const Options&) {}
    static void closeNative(const NativeWindowHandle&) {}
    static void openSecondaryNative(std::string_view, std::string_view, std::string_view page, const Options& options) {
        openedSecondary.push_back(std::string(page) + " " + options.window.title + " " + options.launch.secret);
    }
    static bool closeSecondaryNative(std::string_view) {
        return true;
    }
};

struct AbsentFrontend : EmbeddedFrontend<AbsentFrontend> {
    static constexpr bool             secondaryWindows = true;
    static constexpr bool             compiled         = false;
    static constexpr std::string_view name             = "AbsentFrontend";
    static constexpr std::string_view enable           = "Configure on Nowhere with -DNOWHERE=ON";
    using Host                                         = UnavailableHost;
    using Options                                      = ViewOptions;
};

struct SingleWindowFrontend : EmbeddedFrontend<SingleWindowFrontend> {
    static constexpr bool             secondaryWindows = false;
    static constexpr bool             compiled         = true;
    static constexpr std::string_view name             = "SingleWindowFrontend";
    using Host                                         = SingleWindowFrontend;
    using Options                                      = ViewOptions;
    static void openNative(std::string_view, std::string_view, std::string_view, const Options&) {}
    static void closeNative(const NativeWindowHandle&) {}
};

template <typename Frontend>
constexpr bool offersSecondaryWindow = requires(std::string_view text, const ContentPolicy& policy, const WindowOptions& window, const ViewLaunch& launch) {
    Frontend::openSecondaryWindow(text, text, text, policy, window, launch);
    Frontend::closeSecondaryWindow(text);
};
static_assert(offersSecondaryWindow<FakeFrontend> && !offersSecondaryWindow<SingleWindowFrontend>);
static_assert(FrontendType<FakeFrontend> && FrontendType<AbsentFrontend> && FrontendType<SingleWindowFrontend>);
}  // namespace window_registry_fakes
using namespace window_registry_fakes;

const Scenario embeddedFrontendFacade{
    "The facade of the embedded frontends gives every overload of open()",
    {"window-registry"},
    [] {
        given("a frontend that declares its toolkit calls", [&] {
            FakeFrontend::opened.clear();
            when("it is opened with every form the application uses", [&] {
                FakeFrontend::open("9000", "a.html");
                FakeFrontend::open("1.2.3.4", "9001", "b.html");
                FakeFrontend::open("1.2.3.4", "9002", "c.html", ContentPolicy{});
                FakeFrontend::open("1.2.3.4", "9003", "d.html", ContentPolicy{}, WindowOptions{.title = "T"});
                FakeFrontend::open("1.2.3.4", "9004", "e.html", ContentPolicy{}, WindowOptions{}, ViewLaunch{"secret", "origin"});
                FakeFrontend::open("1.2.3.4", "9005", "f.html", ViewOptions{.window = {.title = "O"}});
                FakeFrontend::open("1.2.3.4", "9006", "g.html", ContentPolicy{.devToolsEnabled = false});
                then("each reaches the toolkit with its settings", [&] {
                    check(FakeFrontend::opened
                          == std::vector<std::string>{"127.0.0.1:9000/a.html",
                                                      "1.2.3.4:9001/b.html",
                                                      "1.2.3.4:9002/c.html",
                                                      "1.2.3.4:9003/d.html T",
                                                      "1.2.3.4:9004/e.html launched",
                                                      "1.2.3.4:9005/f.html O",
                                                      "1.2.3.4:9006/g.html nodevtools"});
                });
            });
            when("no window is open", [&] {
                then("nothing can be closed, and a second window is refused with the frontend's name", [&] {
                    check(!FakeFrontend::requestClose());
                    check(!FakeFrontend::closeSecondaryWindow("a.html"));
                    checkThrows<std::logic_error>(
                        [&] {
                            FakeFrontend::openSecondaryWindow("127.0.0.1", "9000", "a.html", {}, {}, {});
                        },
                        contains("FakeFrontend: no window of the application is open"));
                });
            });
        });
        given("a frontend configured by a provider", [&] {
            ConfiguredFrontend::opened.clear();
            ConfiguredFrontend::open("127.0.0.1", "9000", "a.html", ContentPolicy{});
            ConfiguredFrontend::open("127.0.0.1", "9000", "b.html", ContentPolicy{}, WindowOptions{.title = "own"});
            ConfiguredFrontend::open("127.0.0.1", "9000", "c.html", ContentPolicy{}, WindowOptions{.title = "own"}, ViewLaunch{"own-secret", ""});
            then("a call replaces only the settings it passes", [&] {
                check(ConfiguredFrontend::opened
                      == std::vector<std::string>{"a.html configured configured-secret", "b.html own configured-secret", "c.html own own-secret"});
            });
        });
        given("a configured frontend with a window open", [&] {
            auto& host = uiExecutorHostOf<ConfiguredSecondaryFrontend>();
            host.attach([] {});  // this thread is the interface thread: the calls run on it
            ConfiguredSecondaryFrontend::providerCalls = 0;
            ConfiguredSecondaryFrontend::openedSecondary.clear();
            FakeFrontend::openedSecondary.clear();
            when("a secondary window is opened", [&] {
                ConfiguredSecondaryFrontend::openSecondaryWindow("127.0.0.1",
                                                                 "9000",
                                                                 "a.html",
                                                                 ContentPolicy{},
                                                                 WindowOptions{.title = "second"},
                                                                 ViewLaunch{"secret", ""});
                then("the provider is not asked, and the call's settings arrive", [&] {
                    check(ConfiguredSecondaryFrontend::providerCalls == 0);
                    check(ConfiguredSecondaryFrontend::openedSecondary == std::vector<std::string>{"a.html second secret"});
                });
            });
            host.close();
        });
        given("a frontend with a window open and a policy of its own", [&] {
            auto& host = uiExecutorHostOf<FakeHost>();
            host.attach([] {});
            FakeFrontend::openedSecondary.clear();
            FakeFrontend::openSecondaryWindow("127.0.0.1",
                                              "9000",
                                              "a.html",
                                              ContentPolicy{.devToolsEnabled = false},
                                              WindowOptions{.title = "T"},
                                              ViewLaunch{"s", ""});
            then("the secondary window receives the policy", [&] {
                check(FakeFrontend::openedSecondary == std::vector<std::string>{"a.html nodevtools T s"});
            });
            host.close();
        });
        given("a frontend whose engine is not part of the build", [&] {
            then("every entry point explains what to enable", [&] {
                const auto message = equals("AbsentFrontend is unavailable. Configure on Nowhere with -DNOWHERE=ON");
                checkThrows<std::runtime_error>(
                    [&] {
                        AbsentFrontend::initialize();
                    },
                    message);
                checkThrows<std::runtime_error>(
                    [&] {
                        AbsentFrontend::open("9000", "a.html");
                    },
                    message);
                checkThrows<std::runtime_error>(
                    [&] {
                        AbsentFrontend::openSecondaryWindow("127.0.0.1", "9000", "a.html", {}, {}, {});
                    },
                    message);
                check(!AbsentFrontend::requestClose());
            });
        });
    }};

const Scenario windowPresentation{"A window is made opaque once, by the first frame or by the delay", {"window-registry"}, [] {
                                      given("a presentation", [&] {
                                          frontend::detail::Presentation presentation;
                                          int                            announced = 0;
                                          presentation.windowShown                 = [&] {
                                              ++announced;
                                          };
                                          when("the delay expires and then the page paints", [&] {
                                              check(presentation.begin(false));
                                              presentation.announce();
                                              check(!presentation.begin(true));
                                              then("it was shown once, by the delay", [&] {
                                                  check(announced == 1 && presentation.shown && !presentation.byFrame);
                                              });
                                          });
                                          when("the first frame comes first", [&] {
                                              check(presentation.begin(true));
                                              check(!presentation.begin(false));
                                              then("the frame made it opaque", [&] {
                                                  check(presentation.byFrame);
                                              });
                                          });
                                      });
                                  }};

const Scenario startupTimeoutRange{"A startup deadline a timer cannot hold is refused", {"window-registry"}, [] {
                                       using std::chrono::milliseconds;
                                       then("a positive deadline is kept", [&] {
                                           check(frontend::detail::timerMilliseconds(milliseconds{30000}, "FakeFrontend") == 30000u);
                                       });
                                       then("a facade refuses them before any window is opened", [&] {
                                           FakeFrontend::opened.clear();
                                           ViewOptions options;
                                           options.startupTimeout = milliseconds{-1};
                                           checkThrows<std::invalid_argument>(
                                               [&] {
                                                   FakeFrontend::open("127.0.0.1", "9000", "a.html", options);
                                               },
                                               contains("FakeFrontend: startupTimeout must be positive and at most"));
                                           check(FakeFrontend::opened.empty());
                                       });
                                       then("zero, negative and oversized deadlines are refused with the frontend's name", [&] {
                                           for (const auto bad : {milliseconds{0}, milliseconds{-1}, milliseconds{-86400000}, milliseconds{4294967296LL}})
                                               checkThrows<std::invalid_argument>(
                                                   [&] {
                                                       (void)frontend::detail::timerMilliseconds(bad, "FakeFrontend");
                                                   },
                                                   contains("FakeFrontend: startupTimeout must be positive and at most"));
                                       });
                                   }};
