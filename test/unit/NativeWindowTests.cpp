// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Typed access to the native window of an embedded frontend, and its link to a connection (#463, ADR-020).
#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/frontend/NativeWindow.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/WebFront.hpp>

#include <future>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace webfront;
using namespace webfront::frontend;

namespace webfront::testing {
template <typename Application>
struct LaunchSecret {
    static const std::string& of(const Application& app) { return app.viewSecret; }
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
        executor.attach([] {});
    }
    void publish(std::string portalParent = {}) {
        window.publish({.kind = NativeWindowKind::gtk, .pointer = &windowObject, .portalParent = std::move(portalParent)});
    }
    ~Session() {
        window.retract();
        executor.close();
    }
};

NativeWindowError errorOf(const NativeWindowResult& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

struct PlainTag;
struct LifecycleTag;

// A frontend that publishes a window, and one that has none.
struct WindowFrontend : DefaultBrowserFrontend {
    static NativeWindow nativeWindow() { return nativeWindowHostOf<WindowFrontend>().window(); }
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
    app.onUIStarted([&](typename Application::UI ui) { browser = ui; });
    BrowserSocket::receive(std::as_bytes(std::span(upgradeRequest)));
    msg::Handshake         handshake;
    std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
    for (const char c : proof)
        payload.push_back(static_cast<std::byte>(c));
    BrowserSocket::receive(testing::clientFrame(payload));
    REQUIRE(browser);
    return *browser;
}
}  // namespace

SCENARIO("The native window is given on the interface thread while it is published", "[native-window]") {
    GIVEN("a handle that belongs to no frontend") {
        THEN("it answers unsupported") {
            REQUIRE(errorOf(NativeWindow{}.get()) == NativeWindowError::unsupported);
        }
    }
    GIVEN("a frontend whose window is not created yet") {
        const auto handle = nativeWindowHostOf<LifecycleTag>().window();
        THEN("it answers notOpen") {
            REQUIRE(errorOf(handle.get()) == NativeWindowError::notOpen);
        }
        AND_WHEN("the loop is attached but nothing is published") {
            Session<LifecycleTag> session;
            THEN("it still answers notOpen") {
                REQUIRE(errorOf(handle.get()) == NativeWindowError::notOpen);
            }
        }
    }
    GIVEN("a published window") {
        Session<PlainTag> session;
        const auto        handle = nativeWindowHostOf<PlainTag>().window();
        session.publish("x11:1a2b");
        WHEN("it is read on the interface thread") {
            const auto window = handle.get();
            THEN("it is the toolkit object, with its portal identifier") {
                REQUIRE(window.has_value());
                CHECK(window->kind == NativeWindowKind::gtk);
                CHECK(window->pointer == &windowObject);
                CHECK(window->portalParent == "x11:1a2b");
            }
        }
        WHEN("it is read from another thread") {
            const auto answer = std::async(std::launch::async, [&] { return handle.get(); }).get();
            THEN("it is refused: a toolkit object belongs to the interface thread") {
                REQUIRE(errorOf(answer) == NativeWindowError::wrongThread);
            }
        }
        WHEN("the portal identifier becomes known later") {
            session.window.setPortalParent("wayland:handle");
            THEN("the window carries it") {
                REQUIRE(handle.get()->portalParent == "wayland:handle");
            }
        }
        WHEN("the window is withdrawn") {
            session.window.retract();
            THEN("it answers closed, and a late portal identifier is dropped") {
                REQUIRE(errorOf(handle.get()) == NativeWindowError::closed);
                session.window.setPortalParent("wayland:late");
                REQUIRE(errorOf(handle.get()) == NativeWindowError::closed);
            }
        }
        WHEN("the loop closes without the window being withdrawn") {
            session.executor.close();
            THEN("it is not given to a thread that is no longer the interface thread") {
                REQUIRE(errorOf(handle.get()) == NativeWindowError::wrongThread);
            }
        }
    }
    GIVEN("a window that was published and withdrawn") {
        const auto handle = nativeWindowHostOf<PlainTag>().window();
        {
            Session<PlainTag> first;
            first.publish();
        }
        REQUIRE(errorOf(handle.get()) == NativeWindowError::closed);
        WHEN("a new session publishes a window") {
            Session<PlainTag> second;
            second.publish();
            THEN("the same handle gives it") {
                REQUIRE(handle.get().has_value());
            }
        }
    }
}

SCENARIO("A connection is given a window only when it is the view of the frontend's window", "[native-window][view-association]") {
    freshNetwork();
    App<WindowFrontend> app{WebFrontConfig{.port = "0"}};
    const auto          secret = testing::LaunchSecret<App<WindowFrontend>>::of(app);
    Session<WindowFrontend> session;
    session.publish();

    WHEN("the connection presents the secret of the launch") {
        const auto ui = connect(app, secret);
        THEN("its window is the frontend's") {
            const auto window = ui.nativeWindow();
            REQUIRE(window.has_value());
            CHECK(window->pointer == &windowObject);
        }
        AND_WHEN("the window is withdrawn") {
            session.window.retract();
            THEN("the connection no longer has one") {
                REQUIRE(errorOf(ui.nativeWindow()) == NativeWindowError::closed);
            }
        }
        AND_WHEN("the UI is used from another thread") {
            const auto answer = std::async(std::launch::async, [&] { return ui.nativeWindow(); }).get();
            THEN("it is refused") {
                REQUIRE(errorOf(answer) == NativeWindowError::wrongThread);
            }
        }
    }
    WHEN("the connection is a browser without the secret, whatever its address") {
        const auto ui = connect(app, {});
        THEN("it has no window to parent a dialog to") {
            REQUIRE(errorOf(ui.nativeWindow()) == NativeWindowError::notEmbedded);
        }
    }
}

SCENARIO("A frontend without a native window gives none, even to its embedded view", "[native-window]") {
    freshNetwork();
    App<NoWindowFrontend> app{WebFrontConfig{.port = "0"}};
    CHECK(errorOf(app.nativeWindow().get()) == NativeWindowError::unsupported);
    const auto ui = connect(app, testing::LaunchSecret<App<NoWindowFrontend>>::of(app));
    REQUIRE(ui.isEmbeddedView());
    THEN("the answer is unsupported") {
        REQUIRE(errorOf(ui.nativeWindow()) == NativeWindowError::unsupported);
    }
}

SCENARIO("The browser fallback has no native window", "[native-window]") {
    using Fallback = NativeOrBrowserFrontendFor<DefaultBrowserFrontend, DefaultBrowserFrontend>;
    Fallback::initialize();
    REQUIRE(errorOf(Fallback::nativeWindow().get()) == NativeWindowError::unsupported);
}
