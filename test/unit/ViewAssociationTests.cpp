// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-04
//
// WebFront — tests/unit
// Role: A connection proves it comes from the view of an embedded frontend by the secret of the launch.
// Thread: bridge-io | Alloc: heap-init
// LLM: security tests of ADR-020 rule 3

// A connection proves it comes from the view of an embedded frontend by the secret of the launch (#453, ADR-020).

#include "InjectableNetworking.hpp"
#include "TemporaryDirectory.hpp"
#include <webfront/DeveloperApp.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/tooling/Logger.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;

namespace webfront::testing {
template <typename Application>
struct LaunchSecret {
    static const std::string& of(const Application& app) {
        return app.viewSecret;
    }
};
}  // namespace webfront::testing

namespace {
const std::string viewUpgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

struct ViewOneBrowserNet : testing::InjectableNetworking {
    static inline bool accepted{};
    static auto        Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (std::exchange(accepted, true))
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

using ViewSocket = testing::InjectableSocket;
using ViewApp    = BasicWF<ViewOneBrowserNet, fs::DefaultFS>;

struct ViewVerdict {
    bool embedded = false;
    bool local    = false;
};

// Connects one browser whose handshake carries `proof` after its two header bytes, and reports what the UI says.
template <typename Application = ViewApp>
ViewVerdict connect(Application& app, const std::string& proof, const std::string& request = viewUpgradeRequest) {
    ViewVerdict verdict;
    bool        started = false;
    app.onUIStarted([&](typename Application::UI ui) {
        verdict.embedded = ui.isEmbeddedView();
        verdict.local    = ui.isLocalClient();
        started          = true;
    });
    ViewSocket::receive(std::as_bytes(std::span(request)));
    msg::Handshake         handshake;
    std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
    for (const char c : proof)
        payload.push_back(static_cast<std::byte>(c));
    ViewSocket::receive(testing::clientFrame(payload));
    require(started);
    return verdict;
}

void freshNetwork() {
    ViewSocket::reset();
    ViewSocket::peerIsLoopback  = false;
    ViewOneBrowserNet::accepted = false;
}
}  // namespace

const Scenario connectionAssociated{"A connection is associated with the view of an embedded frontend only by the secret of the launch",
                                    {"view-association"},
                                    [] {
                                        freshNetwork();
                                        ViewApp           app{WebFrontConfig{.port = "0"}};
                                        const std::string secret = testing::LaunchSecret<ViewApp>::of(app);

                                        given("the secret of the launch", [&] {
                                            then("it is 256 random bits in lowercase hexadecimal", [&] {
                                                require(secret.size() == 64);
                                                require(std::ranges::all_of(secret, [](char c) {
                                                    return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c));
                                                }));
                                            });
                                            then("another application has another one", [&] {
                                                ViewApp other{WebFrontConfig{.port = "0"}};
                                                require(testing::LaunchSecret<ViewApp>::of(other) != secret);
                                            });
                                        });
                                        when("the handshake presents the secret", [&] {
                                            const auto verdict = connect(app, secret);
                                            then("the connection is the embedded view and a local client", [&] {
                                                require(verdict.embedded);
                                                require(verdict.local);
                                            });
                                        });
                                        when("the handshake presents a wrong secret of the right length", [&] {
                                            auto wrong         = secret;
                                            wrong.back()       = wrong.back() == '0' ? '1' : '0';
                                            const auto verdict = connect(app, wrong);
                                            then("the connection is neither", [&] {
                                                require(!verdict.embedded);
                                                require(!verdict.local);
                                            });
                                        });
                                        when("the handshake presents a prefix of the secret", [&] {
                                            then("it is not accepted", [&] {
                                                require(!connect(app, secret.substr(0, 63)).embedded);
                                            });
                                        });
                                        when("the handshake presents no secret", [&] {
                                            const auto verdict = connect(app, {});
                                            then("the connection is neither, whatever its address", [&] {
                                                require(!verdict.embedded);
                                                require(!verdict.local);
                                            });
                                        });
                                        when("the handshake carries the secret of an earlier launch", [&] {
                                            std::string earlier;
                                            {
                                                ViewApp previous{WebFrontConfig{.port = "0"}};
                                                earlier = testing::LaunchSecret<ViewApp>::of(previous);
                                            }
                                            const auto verdict = connect(app, earlier);
                                            then("it is rejected: the secret does not survive a restart", [&] {
                                                require(!verdict.embedded);
                                            });
                                        });
                                        when("the handshake carries the secret padded with extra bytes", [&] {
                                            require(!connect(app, secret + "00").embedded);
                                        });
                                        when("the handshake carries a proof longer than a secret", [&] {
                                            require(!connect(app, std::string(100, 'a')).embedded);
                                        });
                                        when("a second handshake without the secret follows the one that presented it", [&] {
                                            std::optional<ViewApp::UI> browser;
                                            app.onUIStarted([&](ViewApp::UI ui) {
                                                browser = ui;
                                            });
                                            ViewSocket::receive(std::as_bytes(std::span(viewUpgradeRequest)));
                                            msg::Handshake         handshake;
                                            std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
                                            for (const char c : secret)
                                                payload.push_back(static_cast<std::byte>(c));
                                            ViewSocket::receive(testing::clientFrame(payload));
                                            require(browser);
                                            require(browser->isEmbeddedView());
                                            ViewSocket::receive(
                                                testing::clientFrame(std::vector<std::byte>(handshake.header().begin(), handshake.header().end())));
                                            then("the connection stays the embedded view", [&] {
                                                require(browser->isEmbeddedView());
                                                require(browser->isLocalClient());
                                            });
                                        });
                                    }};

const Scenario scriptHands{"The script that hands the secret to a view refuses what it cannot write safely", {"view-association"}, [] {
                               const std::string secret(64, 'a');
                               given("a hexadecimal secret and a plain origin", [&] {
                                   const auto script = detail::viewSecretScript(secret, "http://127.0.0.1:9002");
                                   then("the script names the origin and the secret", [&] {
                                       require(script.contains("location.origin === new URL('http://127.0.0.1:9002').origin"));
                                       require(script.contains(secret));
                                   });
                               });
                               given("an origin or a secret that would need escaping", [&] {
                                   then("there is no script, hence no association", [&] {
                                       require(detail::viewSecretScript(secret, "http://x'; alert(1); '").empty());
                                       require(detail::viewSecretScript(secret, "http://x\\").empty());
                                       require(detail::viewSecretScript(secret, "http://x\n").empty());
                                       require(detail::viewSecretScript("a'b", "http://127.0.0.1:9002").empty());
                                       require(detail::viewSecretScript({}, "http://127.0.0.1:9002").empty());
                                   });
                               });
                           }};

const Scenario originGiven{"The origin given to a view is written as the browser writes location.origin", {"view-association"}, [] {
                               then("the default port 80 is dropped and the host is lowercase", [&] {
                                   require(detail::browserOrigin("127.0.0.1", "80") == "http://127.0.0.1");
                                   require(detail::browserOrigin("LocalHost", "9002") == "http://localhost:9002");
                                   require(detail::browserOrigin("127.0.0.1", "8080") == "http://127.0.0.1:8080");
                               });
                           }};

const Scenario iPv4Loopback{"An IPv4 loopback seen through a dual-stack socket is a loopback peer", {"view-association"}, [] {
                                then("the v4-mapped loopback is, another mapped address and a public one are not", [&] {
                                    require(detail::isLoopbackAddress(stdnet::ip::address(stdnet::ip::address_v4::loopback())));
                                    require(detail::isLoopbackAddress(stdnet::ip::address(stdnet::ip::address_v6::loopback())));
                                    const auto mapped = [](unsigned char first) {
                                        stdnet::ip::address_v6::bytes_type bytes{};
                                        bytes[10] = bytes[11] = 0xFF;
                                        bytes[12]             = first;
                                        bytes[15]             = 1;
                                        return stdnet::ip::address(stdnet::ip::address_v6(bytes));
                                    };
                                    require(detail::isLoopbackAddress(mapped(127)));
                                    require(!detail::isLoopbackAddress(mapped(10)));
                                });
                            }};

const Scenario loopbackPeerScenario{"A loopback peer is a local client only when the application trusts loopback clients", {"view-association"}, [] {
                                        freshNetwork();
                                        ViewSocket::peerIsLoopback = true;

                                        given("the default configuration", [&] {
                                            ViewApp    app{WebFrontConfig{.port = "0"}};
                                            const auto verdict = connect(app, {});
                                            then("a loopback peer without the secret is not local", [&] {
                                                require(!verdict.embedded);
                                                require(!verdict.local);
                                            });
                                        });
                                        given("trustLoopbackClients", [&] {
                                            ViewApp app{
                                                WebFrontConfig{.port = "0", .trustLoopbackClients = true}
                                            };
                                            const auto verdict = connect(app, {});
                                            then("a loopback peer is local though it is not the embedded view", [&] {
                                                require(!verdict.embedded);
                                                require(verdict.local);
                                            });
                                        });
                                        given("trustLoopbackClients and a peer that is not the loopback", [&] {
                                            ViewSocket::peerIsLoopback = false;
                                            ViewApp app{
                                                WebFrontConfig{.port = "0", .trustLoopbackClients = true}
                                            };
                                            const auto verdict = connect(app, {});
                                            then("the connection is not local", [&] {
                                                require(!verdict.local);
                                            });
                                        });
                                    }};

const Scenario handshakeSecret{"The handshake's secret never reaches the diagnostics", {"view-association"}, [] {
                                   freshNetwork();
                                   ViewApp                  app{WebFrontConfig{.port = "0"}};
                                   std::vector<std::string> records;
                                   log::setLogLevel(log::Debug);
                                   const auto sink = log::addSinks([&records](std::string_view text) {
                                       records.emplace_back(text);
                                   });

                                   when("a handshake carrying a recognizable proof is received", [&] {
                                       connect(app, std::string(64, 'Q'));
                                       log::removeSinks(sink);
                                       then("the message is logged, but no record contains the proof in text or as a hexadecimal dump", [&] {
                                           require(std::ranges::any_of(records, [](const std::string& record) {
                                               return record.contains("onMessage(binary)");
                                           }));
                                           for (const auto& record : records) {
                                               require(!record.contains("QQQQ"));
                                               require(!record.contains("51 51 51"));
                                               require(!record.contains("515151"));
                                           }
                                       });
                                   });
                                   log::removeSinks(sink);
                               }};

namespace {
/// A frontend that opens no secondary window: the developer environment's own frontend in these tests.
struct InspectorQuietFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
};
using InspectorApp = DeveloperAppWithNetwork<ViewOneBrowserNet, InspectorQuietFrontend>;

/// The browser calls a function without argument; the answer is among the socket's writes.
void callWithoutArgument(const std::string& name, msg::CallId callId) {
    msg::FunctionCall<> message;
    message.setCallId(callId);
    websocket::Frame<ViewOneBrowserNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(name, frame);
    frame.freeze();
    ViewSocket::receive(testing::clientFrame(testing::messagePayload(frame)));
}

/// The encoded answer of webfront.inspector.containers(): a tuple of three booleans.
std::string containersAnswer(bool secondaryWindow, bool host, bool newConnections) {
    const auto boolean = [](bool value) {
        return static_cast<char>(value ? msg::CodedType::booleanTrue : msg::CodedType::booleanFalse);
    };
    return {static_cast<char>(msg::CodedType::tuple), char{3}, boolean(secondaryWindow), boolean(host), boolean(newConnections)};
}

/// The developer environment refuses a WebSocket without an Origin; the test's browser names one that the policy admits.
WebFrontConfig inspectorConfig(bool trustLoopback) {
    WebFrontConfig config{.port = "0", .trustLoopbackClients = trustLoopback};
    config.contentPolicy.allowAnyBridgeOrigin = true;
    return config;
}
const std::string inspectorUpgradeRequest = "GET / HTTP/1.1\r\nHost: localhost\r\nOrigin: http://localhost\r\nUpgrade: websocket\r\n"
                                            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

/// The application of a developer project, bootstrapped, with one browser that presents `proof` and asks for the containers.
struct InspectorBrowser {
    explicit InspectorBrowser(bool trustLoopback) : app(project.path(), inspectorConfig(trustLoopback)) {
        app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"});
    }
    void connectAndAsk(const std::string& proof) {
        connect(app, proof, inspectorUpgradeRequest);
        callWithoutArgument("webfront.inspector.containers", 7);
    }
    webfront::test::TemporaryDirectory project;
    InspectorApp                       app;
};
}  // namespace

const Scenario inspectorContainersScenario{"The inspectors' containers follow the effective access of the calling page (ADR-027, section 8)",
                                           {"view-association"},
                                           [] {
                                               given("the three answers computed from the frontend, the page and the trust", [&] {
                                                   then("a frontend that opens windows offers one only to an admitted page", [&] {
                                                       require(developer::inspectorContainers(true, true, false) == std::tuple{true, true, false});
                                                       require(developer::inspectorContainers(true, false, false) == std::tuple{false, false, false});
                                                       require(developer::inspectorContainers(true, false, true) == std::tuple{false, false, true});
                                                   });
                                                   then("a frontend that opens none offers no window, even to an admitted page", [&] {
                                                       require(developer::inspectorContainers(false, true, true) == std::tuple{false, true, true});
                                                       require(developer::inspectorContainers(false, false, false) == std::tuple{false, false, false});
                                                   });
                                               });
                                               given("a developer application whose frontend opens no secondary window", [&] {
                                                   freshNetwork();
                                                   InspectorBrowser browser{false};
                                                   then("the function is registered once the environment is bootstrapped", [&] {
                                                       require(browser.app.hasCppFunction("webfront.inspector.containers"));
                                                   });
                                                   when("the view presents the secret of the launch", [&] {
                                                       browser.connectAndAsk(testing::LaunchSecret<InspectorApp>::of(browser.app));
                                                       then("the page is admitted, without a secondary window or new connections", [&] {
                                                           require(ViewSocket::wrote(containersAnswer(false, true, false)));
                                                       });
                                                   });
                                               });
                                               given("a page that is not proven, such as the system browser's or a frame's", [&] {
                                                   freshNetwork();
                                                   ViewSocket::peerIsLoopback = true;
                                                   InspectorBrowser browser{false};
                                                   browser.connectAndAsk({});
                                                   then("the call is answered, and no container is admitted", [&] {
                                                       require(ViewSocket::wrote(containersAnswer(false, false, false)));
                                                   });
                                               });
                                               given("trustLoopbackClients and a page that is not proven on the loopback", [&] {
                                                   freshNetwork();
                                                   ViewSocket::peerIsLoopback = true;
                                                   InspectorBrowser browser{true};
                                                   browser.connectAndAsk({});
                                                   then("the page and the new connections are admitted", [&] {
                                                       require(ViewSocket::wrote(containersAnswer(false, true, true)));
                                                   });
                                               });
                                               given("trustLoopbackClients and a page that is not on the loopback", [&] {
                                                   freshNetwork();
                                                   InspectorBrowser browser{true};
                                                   browser.connectAndAsk({});
                                                   then("the page is not admitted, though new loopback connections would be", [&] {
                                                       require(ViewSocket::wrote(containersAnswer(false, false, true)));
                                                   });
                                               });
                                               given("a developer application that is not bootstrapped", [&] {
                                                   freshNetwork();
                                                   webfront::test::TemporaryDirectory project;
                                                   InspectorApp                       app(project.path(), WebFrontConfig{.port = "0"});
                                                   then("it has no inspector function", [&] {
                                                       require(!app.hasCppFunction("webfront.inspector.containers"));
                                                   });
                                               });
                                           }};

namespace {
/// A frontend that opens secondary windows (WebKitGTK, WKWebView, WebView2 in this respect), counting the windows it opens.
struct InspectorWindowFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};
    static void                       initialize() { }
    static void                       open(std::string_view, std::string_view) { }
    static inline int                 opened = 0;
    static void openSecondaryWindow(std::string_view, std::string_view, std::string_view, const ContentPolicy&, const WindowOptions&, const ViewLaunch&) {
        ++opened;
    }
    [[nodiscard]] static frontend::UIExecutor uiExecutor() {
        return {};
    }
};
using InspectorWindowApp = DeveloperAppWithNetwork<ViewOneBrowserNet, InspectorWindowFrontend>;

/// The browser calls `name` with `arguments`; the answer is among the socket's writes.
template <typename... Arguments>
void callWith(const std::string& name, msg::CallId callId, Arguments... arguments) {
    msg::FunctionCall<> message;
    message.setCallId(callId);
    websocket::Frame<ViewOneBrowserNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(name, frame);
    (message.encodeParameter(arguments, frame), ...);
    frame.freeze();
    ViewSocket::receive(testing::clientFrame(testing::messagePayload(frame)));
}

/// A bootstrapped developer application of `Frontend`, with one browser that presented `proof` (or not).
template <typename Application>
struct AdmittedBrowser {
    AdmittedBrowser(bool trustLoopback, bool proven) : app(project.path(), inspectorConfig(trustLoopback)) {
        app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"});
        connect(app, proven ? testing::LaunchSecret<Application>::of(app) : std::string{}, inspectorUpgradeRequest);
    }
    webfront::test::TemporaryDirectory project;
    Application                        app;
};
}  // namespace

const Scenario inspectorContainersWithWindows{
    "A frontend that opens windows offers one only to an admitted page (ADR-027, section 8)",
    {"view-association", "inspector"},
    [] {
        given("the proven view", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorWindowApp> browser{false, true};
            callWithoutArgument("webfront.inspector.containers", 7);
            then("a secondary window and the page are admitted, no new connection", [&] {
                require(ViewSocket::wrote(containersAnswer(true, true, false)));
            });
        });
        given("a page that is not proven, on the loopback, without trustLoopbackClients", [&] {
            freshNetwork();
            ViewSocket::peerIsLoopback = true;
            AdmittedBrowser<InspectorWindowApp> browser{false, false};
            callWithoutArgument("webfront.inspector.containers", 7);
            then("no container: the window would be opened by a page that is refused", [&] {
                require(ViewSocket::wrote(containersAnswer(false, false, false)));
            });
        });
        given("a page that is not proven, on the loopback, with trustLoopbackClients", [&] {
            freshNetwork();
            ViewSocket::peerIsLoopback = true;
            AdmittedBrowser<InspectorWindowApp> browser{true, false};
            callWithoutArgument("webfront.inspector.containers", 7);
            then("the trusted page may open a window, and new connections are admitted", [&] {
                require(ViewSocket::wrote(containersAnswer(true, true, true)));
            });
        });
      }
};

const Scenario inspectorWindowAccess{
    "webfront.inspector.openWindow opens a panel's window only for an admitted page (ADR-027, sections 2 and 8)",
    {"view-association", "inspector"},
    [] {
        given("a page that is not admitted, with a frontend that opens windows", [&] {
            freshNetwork();
            InspectorWindowFrontend::opened = 0;
            AdmittedBrowser<InspectorWindowApp> browser{false, false};
            callWith("webfront.inspector.openWindow", 9, std::string{"cpp.watches"}, std::string{"host-1"}, 720.0, 560.0);
            then("the call is refused as denied, and no window opens", [&] {
                require(ViewSocket::wrote(inspect::code::denied));
                requireEqual(InspectorWindowFrontend::opened, 0);
            });
        });
        given("the proven view and an unknown panel", [&] {
            freshNetwork();
            InspectorWindowFrontend::opened = 0;
            AdmittedBrowser<InspectorWindowApp> browser{false, true};
            callWith("webfront.inspector.openWindow", 9, std::string{"../../etc/passwd"}, std::string{"host-1"}, 720.0, 560.0);
            then("the call is refused before any window", [&] {
                require(ViewSocket::wrote("Unknown inspector panel"));
                requireEqual(InspectorWindowFrontend::opened, 0);
            });
        });
        given("the proven view of a frontend that opens no secondary window", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, true};
            callWith("webfront.inspector.openWindow", 9, std::string{"cpp.watches"}, std::string{"host-1"}, 720.0, 560.0);
            then("the call is refused as unavailable", [&] {
                require(ViewSocket::wrote(inspect::code::unavailable));
            });
        });
        given("the pages of a panel", [&] {
            then("each panel of each host page has its own window, and only the panels of the menu have one", [&] {
                requireEqual(developer::inspectorPage("cpp.watches", "a1_b-2"),
                             std::string{"webfront/developer/environment/inspector.html?panel=cpp.watches&host=a1_b-2"});
                checkThrows<std::invalid_argument>([] {
                    static_cast<void>(developer::inspectorPage("cpp", "host"));
                });
                // The host page names itself in the window's address: no other text reaches the page string.
                for (const auto* host : {"", "a/b", "a&panel=x", "é", "x x"})
                    checkThrows<std::invalid_argument>([host] {
                        static_cast<void>(developer::inspectorPage("cpp.watches", host));
                    });
                require(developer::inspectorHostIdentifier(std::string(64, 'a')) && !developer::inspectorHostIdentifier(std::string(65, 'a')));
                requireEqual(developer::inspectorWindowExtent(10.0, 720), 720);
                requireEqual(developer::inspectorWindowExtent(800.5, 720), 800);
            });
        });
      }
};

const Scenario inspectorWatchesAccess{
    "webfront.inspector.watches lists the application's watches to an admitted page only (ADR-027, sections 2 and 3)",
    {"view-association", "inspector"},
    [] {
        std::vector<int> grid{1, 2, 3};
        given("the proven view and a watch declared after bootstrap", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, true};
            auto                          watch = browser.app.watch("grid", [&grid]() -> const std::vector<int>& {
                return grid;
            });
            require(static_cast<bool>(watch));
            callWith("webfront.inspector.watches", 11, 0.0, 0.0, 200.0);
            then("the page names the watch, its kind and where it was declared", [&] {
                require(ViewSocket::wrote("grid"));
                require(ViewSocket::wrote("sequence"));
                require(ViewSocket::wrote("ViewAssociationTests.cpp:"));
            });
            then("the page asks whether this page is an embedded view", [&] {
                callWithoutArgument("webfront.inspector.embedded", 12);
                require(!ViewSocket::wrote(inspect::code::denied));
            });
            then("a second watch of the same name is refused while the first lives", [&] {
                checkThrows<std::invalid_argument>([&] {
                    static_cast<void>(browser.app.watch("grid", [&grid]() -> const std::vector<int>& {
                        return grid;
                    }));
                });
            });
        });
        given("a page that is not admitted", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, false};
            auto                          watch = browser.app.watch("secret", [&grid]() -> const std::vector<int>& {
                return grid;
            });
            callWith("webfront.inspector.watches", 11, 0.0, 0.0, 200.0);
            then("the call is refused as denied, before any read of the table", [&] {
                require(ViewSocket::wrote(inspect::code::denied));
                require(!ViewSocket::wrote("secret"));
            });
        });
        given("a page that is not admitted asks whether it is an embedded view", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, false};
            callWithoutArgument("webfront.inspector.embedded", 12);
            then("the call is refused as denied", [&] {
                require(ViewSocket::wrote(inspect::code::denied));
            });
        });
        given("a trusted loopback page", [&] {
            freshNetwork();
            ViewSocket::peerIsLoopback = true;
            AdmittedBrowser<InspectorApp> browser{true, false};
            auto                          watch = browser.app.watch("trusted", [&grid]() -> const std::vector<int>& {
                return grid;
            });
            callWith("webfront.inspector.watches", 11, 0.0, 0.0, 200.0);
            then("it is admitted by trustLoopbackClients", [&] {
                require(ViewSocket::wrote("trusted"));
            });
        });
      }
};
