// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-09
//
// WebFront — tests/unit
// Role: Typed access to the native window of an embedded frontend and its link to a connection.
// Thread: ui-loop | Alloc: heap-init
// LLM: pins notOpen, closed and wrongThread answers

// Typed access to the native window of an embedded frontend, and its link to a connection (#463, ADR-020).

#include "InjectableNetworking.hpp"
#include <webfront/frontend/NativeWindow.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#include <future>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;
using namespace webfront::frontend;

namespace webfront::testing {
template <typename Application>
struct LaunchSecret {
    static const std::string& of(const Application& app) {
        return app.viewSecret;
    }
};
}  // namespace webfront::testing

namespace {
int windowObject;  // only its address is used: a stand-in for a toolkit object

// What a frontend's open() does around its loop: this thread becomes the interface thread, then the window is published.
template <typename Tag>
struct Session {
    UIExecutorHost&   executor = uiExecutorHostOf<Tag>();
    NativeWindowHost& window   = nativeWindowHostOf<Tag>();
    Session() {
        executor.attach([] { });
    }
    std::uint64_t publish(std::string portalParent = {}) {
        return window.publish({.kind = NativeWindowKind::gtk, .pointer = &windowObject, .portalParent = std::move(portalParent)});
    }
    ~Session() {
        window.retract();
        executor.close();
    }
};

NativeWindowError errorOf(const NativeWindowResult& result) {
    require(!result.has_value());
    return result.error();
}

struct PlainTag;
struct LifecycleTag;

// A frontend that publishes a window, and one that has none.
struct WindowFrontend : DefaultBrowserFrontend {
    static NativeWindow nativeWindow() {
        return nativeWindowHostOf<WindowFrontend>().window();
    }
};
struct NoWindowFrontend : DefaultBrowserFrontend { };

const std::string upgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

struct OneBrowserNet : testing::InjectableNetworking {
    static inline bool accepted{};
    static auto        Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (std::exchange(accepted, true))
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};
using BrowserSocket = testing::InjectableSocket;

template <typename Frontend>
using App = BasicWF<OneBrowserNet, fs::DefaultFS, http::DefaultBuffersPolicy, Frontend>;

// The injected network is reset before the application is built, which starts accepting at once.
void freshNetwork() {
    BrowserSocket::reset();
    BrowserSocket::peerIsLoopback = false;
    OneBrowserNet::accepted       = false;
}

// Connects one browser whose handshake carries `proof`; returns its UI.
template <typename Application>
typename Application::UI connect(Application& app, const std::string& proof) {
    std::optional<typename Application::UI> browser;
    app.onUIStarted([&](typename Application::UI ui) {
        browser = ui;
    });
    BrowserSocket::receive(std::as_bytes(std::span(upgradeRequest)));
    msg::Handshake         handshake;
    std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
    for (const char c : proof)
        payload.push_back(static_cast<std::byte>(c));
    BrowserSocket::receive(testing::clientFrame(payload));
    require(browser);
    return *browser;
}
}  // namespace

const Scenario nativeWindowGiven{"The native window is given on the interface thread while it is published", {"native-window"}, [] {
                                     given("a handle that belongs to no frontend", [&] {
                                         then("it answers unsupported", [&] {
                                             require(errorOf(NativeWindow{}.get()) == NativeWindowError::unsupported);
                                         });
                                     });
                                     given("a frontend whose window is not created yet", [&] {
                                         const auto handle = nativeWindowHostOf<LifecycleTag>().window();
                                         then("it answers notOpen", [&] {
                                             require(errorOf(handle.get()) == NativeWindowError::notOpen);
                                         });
                                         andWhen("the loop is attached but nothing is published", [&] {
                                             Session<LifecycleTag> session;
                                             then("it still answers notOpen", [&] {
                                                 require(errorOf(handle.get()) == NativeWindowError::notOpen);
                                             });
                                         });
                                     });
                                     given("a published window", [&] {
                                         Session<PlainTag> session;
                                         const auto        handle     = nativeWindowHostOf<PlainTag>().window();
                                         const auto        generation = session.publish("x11:1a2b");
                                         when("it is read on the interface thread", [&] {
                                             const auto window = handle.get();
                                             then("it is the toolkit object, with its portal identifier", [&] {
                                                 require(window.has_value());
                                                 check(window->kind == NativeWindowKind::gtk);
                                                 check(window->pointer == &windowObject);
                                                 check(window->portalParent == "x11:1a2b");
                                             });
                                         });
                                         when("it is read from another thread", [&] {
                                             const auto answer = std::async(std::launch::async, [&] {
                                                                     return handle.get();
                                                                 }).get();
                                             then("it is refused: a toolkit object belongs to the interface thread", [&] {
                                                 require(errorOf(answer) == NativeWindowError::wrongThread);
                                             });
                                         });
                                         when("the portal identifier becomes known later", [&] {
                                             session.window.setPortalParent("wayland:handle", generation);
                                             then("the window carries it", [&] {
                                                 require(handle.get()->portalParent == "wayland:handle");
                                             });
                                         });
                                         when("the window is withdrawn", [&] {
                                             session.window.retract();
                                             then("it answers closed, and a late portal identifier is dropped", [&] {
                                                 require(errorOf(handle.get()) == NativeWindowError::closed);
                                                 session.window.setPortalParent("wayland:late", generation);
                                                 require(errorOf(handle.get()) == NativeWindowError::closed);
                                             });
                                         });
                                         when("the loop closes without the window being withdrawn", [&] {
                                             session.executor.close();
                                             then("it is not given to a thread that is no longer the interface thread", [&] {
                                                 require(errorOf(handle.get()) == NativeWindowError::wrongThread);
                                             });
                                         });
                                     });
                                     given("an asynchronous portal export that answers after its window was replaced", [&] {
                                         const auto    handle = nativeWindowHostOf<PlainTag>().window();
                                         std::uint64_t first{};
                                         {
                                             Session<PlainTag> a;
                                             first = a.publish();  // window A asks for its export, which has not answered yet
                                         }
                                         Session<PlainTag> b;
                                         const auto        second = b.publish("x11:b");
                                         require(second != first);
                                         when("the answer about window A arrives", [&] {
                                             b.window.setPortalParent("wayland:stale-a", first);
                                             then("window B keeps its own identifier", [&] {
                                                 require(handle.get()->portalParent == "x11:b");
                                             });
                                         });
                                         when("the answer about window B arrives", [&] {
                                             b.window.setPortalParent("wayland:b", second);
                                             then("window B takes it", [&] {
                                                 require(handle.get()->portalParent == "wayland:b");
                                             });
                                         });
                                         when("window B is withdrawn and the answer about window A arrives", [&] {
                                             then("it is dropped as well", [&] {
                                                 b.window.retract();
                                                 b.window.setPortalParent("wayland:stale-a", first);
                                                 require(errorOf(handle.get()) == NativeWindowError::closed);
                                             });
                                         });
                                     });
                                     given("a window that was published and withdrawn", [&] {
                                         const auto handle = nativeWindowHostOf<PlainTag>().window();
                                         {
                                             Session<PlainTag> first;
                                             first.publish();
                                         }
                                         require(errorOf(handle.get()) == NativeWindowError::closed);
                                         when("a new session publishes a window", [&] {
                                             Session<PlainTag> second;
                                             second.publish();
                                             then("the same handle gives it", [&] {
                                                 require(handle.get().has_value());
                                             });
                                         });
                                     });
                                 }};

const Scenario connectionGiven{
    "A connection is given a window only when it is the view of the frontend's window",
    {"native-window", "view-association"},
    [] {
        freshNetwork();
        App<WindowFrontend>     app{WebFrontConfig{.port = "0"}};
        const auto              secret = testing::LaunchSecret<App<WindowFrontend>>::of(app);
        Session<WindowFrontend> session;
        session.publish();

        when("the connection presents the secret of the launch", [&] {
            const auto ui = connect(app, secret);
            then("its window is the frontend's", [&] {
                const auto window = ui.nativeWindow();
                require(window.has_value());
                check(window->pointer == &windowObject);
            });
            andWhen("the window is withdrawn", [&] {
                session.window.retract();
                then("the connection no longer has one", [&] {
                    require(errorOf(ui.nativeWindow()) == NativeWindowError::closed);
                });
            });
            andWhen("the UI is used from another thread", [&] {
                const auto answer = std::async(std::launch::async, [&] {
                                        return ui.nativeWindow();
                                    }).get();
                then("it is refused", [&] {
                    require(errorOf(answer) == NativeWindowError::wrongThread);
                });
            });
        });
        when("the connection is a browser without the secret, whatever its address", [&] {
            const auto ui = connect(app, {});
            then("it has no window to parent a dialog to", [&] {
                require(errorOf(ui.nativeWindow()) == NativeWindowError::notEmbedded);
            });
        });
      }
};

const Scenario frontendWithout{"A frontend without a native window gives none, even to its embedded view", {"native-window"}, [] {
                                   freshNetwork();
                                   App<NoWindowFrontend> app{WebFrontConfig{.port = "0"}};
                                   check(errorOf(app.nativeWindow().get()) == NativeWindowError::unsupported);
                                   const auto ui = connect(app, testing::LaunchSecret<App<NoWindowFrontend>>::of(app));
                                   require(ui.isEmbeddedView());
                                   then("the answer is unsupported", [&] {
                                       require(errorOf(ui.nativeWindow()) == NativeWindowError::unsupported);
                                   });
                               }};

const Scenario browserFallback{"The browser fallback has no native window", {"native-window"}, [] {
                                   using Fallback = NativeOrBrowserFrontendFor<DefaultBrowserFrontend, DefaultBrowserFrontend>;
                                   Fallback::initialize();
                                   require(errorOf(Fallback::nativeWindow().get()) == NativeWindowError::unsupported);
                               }};
