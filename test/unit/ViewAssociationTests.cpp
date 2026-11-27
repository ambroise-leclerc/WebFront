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
#include <webfront/Commands.hpp>
#include <webfront/DeveloperApp.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/tooling/Logger.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <cctype>
#include <future>
#include <optional>
#include <span>
#include <stdexcept>
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
    static constexpr bool providesViewProof() noexcept {
        return true;
    }
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

/// The encoded answer of a call that returns a boolean: the header of the return, then the boolean.
std::string booleanAnswer(msg::CallId callId, bool value) {
    const msg::FunctionCallHeader<msg::Command::functionReturn> header{.parametersCount = 1, .callId = callId, .parametersDataSize = 1};
    std::string                                                 bytes(reinterpret_cast<const char*>(&header), sizeof header);
    bytes += static_cast<char>(value ? msg::CodedType::booleanTrue : msg::CodedType::booleanFalse);
    return bytes;
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

const Scenario developerWriteAccess{
    "Developer writes and commands require an admitted client when the frontend proves its view",
    {"view-association", "inspector"},
    [] {
        const std::string               path = "protected.txt";
        const std::vector<std::uint8_t> bytes{'o', 'k'};
        given("an unproven client of an embedded frontend", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorWindowApp> browser{false, false};
            Commands                            commands{browser.app};
            bool                                ran = false;
            commands.add("deploy", "writes the project", [&ran](Commands<InspectorWindowApp>::Context) -> Task {
                ran = true;
                co_return;
            });
            const auto denied = [&](auto name, auto... arguments) {
                const auto offset = ViewSocket::writtenText().size();
                callWith(name, 7, arguments...);
                require(ViewSocket::writtenText().find(inspect::code::denied, offset) != std::string::npos);
            };
            denied("webfront.developer.begin", path, false);
            denied("webfront.developer.beginNew", path, false);
            denied("webfront.developer.write", path, std::string{"output"}, std::uint64_t{0}, bytes);
            denied("webfront.developer.commit", path);
            denied("webfront.developer.abort", path);
            denied("webfront.developer.commitGroup", path, path, path);
            denied("webfront.commands.start", std::uint32_t{1}, path, std::string{"[]"});
            denied("webfront.commands.cancel", std::uint32_t{1});
            denied("webfront.inspector.savePreferences", path, bytes);
            // A command that exists is refused like one that does not: it is neither started nor left running.
            denied("webfront.commands.start", std::uint32_t{2}, std::string{"deploy"}, std::string{"[]"});
            require(!ran);
            requireEqual(commands.running(), std::size_t{0});
            const auto offset = ViewSocket::writtenText().size();
            callWith("webfront.developer.state", 7, path);
            const auto response = ViewSocket::writtenText().substr(offset);
            require(!response.empty() && !response.contains(inspect::code::denied));
            require(!std::filesystem::exists(browser.project.path() / ".webfront/developer/staging"));
            // The names and the helps of the commands stay readable: they change nothing (ADR-027, section 2).
            const auto listed = ViewSocket::writtenText().size();
            callWithoutArgument("webfront.commands.list", 8);
            const auto list = ViewSocket::writtenText().substr(listed);
            require(list.contains("deploy") && !list.contains(inspect::code::denied));
        });
        given("a proven client", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorWindowApp> browser{false, true};
            Commands                            commands{browser.app};
            bool                                ran = false;
            commands.add("deploy", "writes the project", [&ran](Commands<InspectorWindowApp>::Context) -> Task {
                ran = true;
                co_return;
            });
            callWith("webfront.developer.begin", 7, path, false);
            require(!ViewSocket::wrote(inspect::code::denied));
            require(std::filesystem::is_directory(browser.project.path() / ".webfront/developer/staging"));
            callWith("webfront.commands.start", 8, std::uint32_t{1}, std::string{"deploy"}, std::string{"[]"});
            require(ran);
            require(!ViewSocket::wrote(inspect::code::denied));
        });
        given("a system browser with the default trust policy", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, false};
            callWith("webfront.developer.begin", 7, path, false);
            require(!ViewSocket::wrote(inspect::code::denied));
            require(std::filesystem::is_directory(browser.project.path() / ".webfront/developer/staging"));
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

const Scenario inspectorTreeAccess{
    "The trees of the watches are read and written through the bridge by an admitted page only (ADR-027, sections 2, 3 and 6)",
    {"view-association", "inspector"},
    [] {
        std::vector<int> grid{1, 2, 3};
        const auto       visitor = [&grid](inspect::Visit<std::vector<int>> visit) {
            visit(grid);
        };
        given("the proven view and a writable watch", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, true};
            auto                          watch = browser.app.watch("grid", visitor);
            const auto                    id    = static_cast<double>(watch.id());
            then("the functions of the trees are registered", [&] {
                for (const auto* name : {"webfront.inspector.limits",
                                         "webfront.inspector.children",
                                         "webfront.inspector.changes",
                                         "webfront.inspector.value",
                                         "webfront.inspector.fold",
                                         "webfront.inspector.reset",
                                         "webfront.inspector.write"})
                    require(browser.app.hasCppFunction(name));
            });
            then("the root and its children are read, a value is written, and the next pass is empty", [&] {
                callWith("webfront.inspector.reset", 11, id);  // the memory of a first call has the revision 1: reset gives 2
                callWith("webfront.inspector.children", 12, id, std::string{}, 2.0, 0.0, 200.0);
                require(ViewSocket::wrote("sequence"));
                require(ViewSocket::wrote("/#2"));
                require(ViewSocket::wrote("[2]"));
                callWith("webfront.inspector.write", 13, id, 3.0, std::string{"/#1"}, std::string{"2"}, std::string{"20"});
                requireEqual(grid, std::vector<int>({1, 20, 3}));
                callWith("webfront.inspector.value", 14, id, std::string{"/#1"}, 0.0, 0.0);
                callWith("webfront.inspector.changes", 15, id, 4.0, 0.0, 200.0);
                callWith("webfront.inspector.fold", 16, id, std::string{}, 5.0);
                require(!ViewSocket::wrote(std::string_view{"webfront.inspector."}));  // no refusal among the answers
            });
            then("a watch on an executor that has no running loop answers unavailable, and its source is not called", [&] {
                bool read    = false;
                auto waiting = browser.app.watch(
                    "waiting",
                    [&]() -> std::vector<int>& {
                        read = true;
                        return grid;
                    },
                    browser.app.uiExecutor());
                callWith("webfront.inspector.reset", 11, static_cast<double>(waiting.id()));
                require(ViewSocket::wrote(inspect::code::unavailable));
                require(!read);
            });
            then("a call for a watch that is gone is refused as unknown", [&] {
                watch.reset();
                callWith("webfront.inspector.reset", 11, id);
                require(ViewSocket::wrote(inspect::code::unknown));
            });
        });
        given("a page that is not admitted", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, false};
            auto                          watch = browser.app.watch("grid", visitor);
            const auto                    id    = static_cast<double>(watch.id());
            then("every function of the trees is refused as denied, and nothing is read or written", [&] {
                callWithoutArgument("webfront.inspector.limits", 10);
                callWith("webfront.inspector.reset", 11, id);
                callWith("webfront.inspector.children", 12, id, std::string{}, 2.0, 0.0, 200.0);
                callWith("webfront.inspector.changes", 13, id, 2.0, 0.0, 200.0);
                callWith("webfront.inspector.value", 14, id, std::string{}, 0.0, 0.0);
                callWith("webfront.inspector.fold", 15, id, std::string{}, 2.0);
                callWith("webfront.inspector.write", 16, id, 2.0, std::string{"/#1"}, std::string{"2"}, std::string{"20"});
                require(ViewSocket::wrote(inspect::code::denied));
                require(!ViewSocket::wrote("sequence"));
                requireEqual(grid, std::vector<int>({1, 2, 3}));
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
            then("the proven view is told that it is an embedded view, which never offers it a tab", [&] {
                callWithoutArgument("webfront.inspector.embedded", 12);
                require(ViewSocket::wrote(booleanAnswer(12, true)));
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
            then("the call is refused as denied, without an answer", [&] {
                require(ViewSocket::wrote(inspect::code::denied));
                require(!ViewSocket::wrote(booleanAnswer(12, false)) && !ViewSocket::wrote(booleanAnswer(12, true)));
            });
        });
        given("a trusted loopback page that is not proven asks whether it is an embedded view", [&] {
            freshNetwork();
            ViewSocket::peerIsLoopback = true;
            AdmittedBrowser<InspectorApp> browser{true, false};
            callWithoutArgument("webfront.inspector.embedded", 12);
            then("under a frontend without the proof, it is told that it is not one: a tab of the system browser may be offered", [&] {
                require(ViewSocket::wrote(booleanAnswer(12, false)));
            });
        });
        given("a trusted loopback page that is not proven, under a frontend that gives its views the proof", [&] {
            freshNetwork();
            ViewSocket::peerIsLoopback = true;
            AdmittedBrowser<InspectorWindowApp> browser{true, false};
            callWithoutArgument("webfront.inspector.embedded", 12);
            then("it is told that it may be one (a frame of an embedded view), which never offers it a tab", [&] {
                require(ViewSocket::wrote(booleanAnswer(12, true)));
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

namespace {
/// A value as the bridge encodes it in an answer: what a page of the inspectors holds for it.
template <typename Value>
std::string encodedValue(const Value& value) {
    msg::FunctionReturn<>               message;
    websocket::Frame<ViewOneBrowserNet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(value, frame);
    frame.freeze();
    const auto  bytes = testing::messagePayload(frame);
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return text.substr(message.header().size());
}

/// A receiver that drops what a typed call completes with: the test reads the inspectors' counters instead.
struct IgnoringReceiver {
    using receiver_concept = networking::ex::receiver_tag;
    void set_value(int) noexcept { }
    void set_error(std::exception_ptr) noexcept { }
    void set_stopped() noexcept { }
};

/// What the socket wrote in answer to `ask`.
template <typename Ask>
std::string answerOf(Ask ask) {
    const auto offset = ViewSocket::writtenText().size();
    ask();
    return ViewSocket::writtenText().substr(offset);
}
}  // namespace

const Scenario inspectorBridgeRelay{
    "The state of a page's bridge is published and read by admitted pages only, with the existing wire types (#684, ADR-003)",
    {"view-association", "inspector", "bridge-state"},
    [] {
        using Rows = std::vector<inspect::BridgeRow>;
        // Two rows: the test socket carries client frames of less than 126 bytes.
        const Rows rows{
            {                  "pending",                  "save",                  "",                  7.0,                  125.5},
            {                  "js",                  "draw",                  "\xC3\xA9",                  9.0,       3.0}};
       given("the proven view of a bootstrapped application", [&] {
       freshNetwork();
       AdmittedBrowser<InspectorApp> browser{false, true};
       then("the three functions of the relay exist", [&] {
       for (const auto* name : {"webfront.inspector.bridgeSource", "webfront.inspector.publishBridge", "webfront.inspector.bridge"})
       require(browser.app.hasCppFunction(name));
       });
       when("it announces the state of its bridge while nobody reads", [&] {
       callWith("webfront.inspector.bridgeSource", 21, std::string{"h1"});
       then("it is told not to publish", [&] {
       require(ViewSocket::wrote(booleanAnswer(21, false)));
       require(!ViewSocket::writtenText().contains("wfInspect.publish"));
       });
       andWhen("an inspector reads that state", [&] {
       const auto empty = answerOf([] {
       callWith("webfront.inspector.bridge", 22, std::string{"h1"}, 0.0, 200.0);
       });
       then("the page is asked to publish, by a call to its JavaScript function", [&] {
       require(empty.contains("wfInspect.publish"));
       require(!empty.contains(inspect::code::denied));
       });
       andWhen("the page publishes its rows", [&] {
       callWith("webfront.inspector.publishBridge", 23, std::string{"h1"}, rows);
       then("it is told to go on publishing", [&] {
       require(ViewSocket::wrote(booleanAnswer(23, true)));
       });
       const auto answer = answerOf([] {
       callWith("webfront.inspector.bridge", 24, std::string{"h1"}, 0.0, 200.0);
       });
       then("the inspector reads the same rows back, encoded as a list of tuples", [&] {
       require(answer.contains(encodedValue(rows)));
       require(!answer.contains("wfInspect.publish"));
       });
       });
       });
       });
       when("it publishes without having announced itself", [&] {
       const auto answer = answerOf([&] {
       callWith("webfront.inspector.publishBridge", 25, std::string{"h2"}, rows);
       });
       then("the publication is refused as unknown", [&] {
       require(answer.contains(inspect::code::unknown));
       });
       });
       when("it names a host with an identifier that is not one", [&] {
       const auto answer = answerOf([] {
       callWith("webfront.inspector.bridge", 26, std::string{"../host"}, 0.0, 200.0);
       });
       then("the call is refused", [&] {
       require(answer.contains("Invalid inspector host identifier"));
       });
       });
       });
       given("a page that is not admitted, such as the frame of the interface or an external browser", [&] {
       freshNetwork();
       AdmittedBrowser<InspectorApp> browser{false, false};
       then("it can neither announce, nor publish, nor read a state", [&] {
       require(answerOf([] {
       callWith("webfront.inspector.bridgeSource", 21, std::string{"h1"});
       }).contains(inspect::code::denied));
       require(answerOf([&] {
       callWith("webfront.inspector.publishBridge", 22, std::string{"h1"}, rows);
       }).contains(inspect::code::denied));
       require(answerOf([] {
       callWith("webfront.inspector.bridge", 23, std::string{"h1"}, 0.0, 200.0);
       }).contains(inspect::code::denied));
       });
       });                                             }
};

const Scenario inspectorRegistryAccess{
    "The functions, the JavaScript calls and the browsers are told to an admitted page only (ADR-027, sections 2 and 7)",
    {"view-association", "inspector"},
    [] {
        given("the proven view of an application that registered a function", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, true};
            browser.app.cppFunction("ping", [](int) {
                return std::string{"pong"};
            });
            then("the three functions exist once the environment is bootstrapped", [&] {
                for (const auto* name : {"webfront.inspector.functions", "webfront.inspector.jsFunctions", "webfront.inspector.browsers"})
                    require(browser.app.hasCppFunction(name));
            });
            when("it asks for the functions", [&] {
                const auto answer = answerOf([] {
                    callWith("webfront.inspector.functions", 11, 0.0, 200.0);
                });
                then("the page names the function, its signature and where it was registered", [&] {
                    require(answer.contains("ping"));
                    require(answer.contains("(a: number) => Promise<string>"));
                    require(answer.contains("ViewAssociationTests.cpp:"));
                });
                then("the environment's own callbacks are listed, the inspectors' are not", [&] {
                    require(answer.contains("webfront.developer.begin"));
                    require(!answer.contains("webfront.inspector."));
                });
            });
            when("a call is made before any inspector read the functions, then one after", [&] {
                const auto ping = [&] {
                    for (const auto& info : browser.app.registeredFunctions())
                        if (info.name == "ping")
                            return info;
                    throw std::logic_error("ping is not registered");
                };
                callWith("ping", 3, 1);
                then("the first is counted, not timed", [&] {
                    requireEqual(ping().calls, std::uint64_t{1});
                    requireEqual(ping().timedCalls, std::uint64_t{0});
                });
                callWith("webfront.inspector.functions", 11, 0.0, 200.0);
                callWith("ping", 4, 1);
                then("the second is timed: an inspector is reading", [&] {
                    requireEqual(ping().calls, std::uint64_t{2});
                    requireEqual(ping().timedCalls, std::uint64_t{1});
                });
            });
            when("it asks for the JavaScript functions that C++ called", [&] {
                browser.app.forEachUI([](InspectorApp::UI ui) {
                    ui.jsFunction("draw")(1);
                    ui.jsFunction("draw")(2);
                    static_cast<void>(ui.template jsFunction<int>("measure")());
                });
                const auto answer = answerOf([] {
                    callWith("webfront.inspector.jsFunctions", 12, 0.0, 200.0);
                });
                then("each is listed with the number of its calls", [&] {
                    require(answer.contains(encodedValue(std::tuple{std::string{"draw"}, 2.0, 0.0}).substr(2)));
                    require(answer.contains(encodedValue(std::tuple{std::string{"measure"}, 1.0, 0.0}).substr(2)));
                });
            });
            when("it asks for the browsers", [&] {
                const auto answer = answerOf([] {
                    callWith("webfront.inspector.browsers", 13, 0.0, 200.0);
                });
                then("it finds itself: an embedded view, local, started, at the root", [&] {
                    using namespace inspect::browser_flag;
                    const auto row = encodedValue(std::tuple{0.0, double{embeddedView | localClient | started | caller}, std::string{"/"}});
                    require(answer.contains(row.substr(2)));
                    require(!answer.contains(inspect::code::denied));
                });
            });
        });
        given("the typed call of a browser that fails", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, true};
            std::future<int>              result;
            browser.app.forEachUI([&](InspectorApp::UI ui) {
                result = ui.template jsFunction<int>("measure")();
            });
            ViewSocket::fail(std::make_error_code(std::errc::connection_reset));
            then("its future fails, and the failure is counted for that function", [&] {
                checkThrows<std::exception>([&] {
                    static_cast<void>(result.get());
                });
                const auto counter = browser.app.jsCallCounter("measure");
                require(counter != nullptr);
                requireEqual(counter->calls.load(), std::uint64_t{1});
                requireEqual(counter->failures.load(), std::uint64_t{1});
            });
        });
        given("a sender built before the developer environment is mounted, and started after", [&] {
            freshNetwork();
            webfront::test::TemporaryDirectory project;
            InspectorApp                       app(project.path(), inspectorConfig(false));
            connect(app, testing::LaunchSecret<InspectorApp>::of(app), inspectorUpgradeRequest);
            std::optional<decltype(std::declval<InspectorApp::UI>().jsFunctionSender<int>(""))> early;
            app.forEachUI([&](InspectorApp::UI ui) {
                early.emplace(ui.template jsFunctionSender<int>("measureLater"));
            });
            require(early.has_value());
            require(app.jsCallCounter("measureLater") == nullptr);
            app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"});
            auto operation = networking::ex::connect((*early)(), IgnoringReceiver{});
            networking::ex::start(operation);
            then("the call it starts is counted: the counter is looked for when the call starts", [&] {
                const auto counter = app.jsCallCounter("measureLater");
                require(counter != nullptr);
                requireEqual(counter->calls.load(), std::uint64_t{1});
                requireEqual(counter->failures.load(), std::uint64_t{0});
            });
            andWhen("the browser disconnects before it answers", [&] {
                ViewSocket::fail(std::make_error_code(std::errc::connection_reset));
                then("the failure of the call is counted too", [&] {
                    requireEqual(app.jsCallCounter("measureLater")->failures.load(), std::uint64_t{1});
                });
            });
        });
        given("a page that is not admitted, such as an external browser's", [&] {
            freshNetwork();
            AdmittedBrowser<InspectorApp> browser{false, false};
            browser.app.cppFunction("ping", [](int) {
                return std::string{"pong"};
            });
            for (const auto* name : {"webfront.inspector.functions", "webfront.inspector.jsFunctions", "webfront.inspector.browsers"}) {
                const auto answer = answerOf([name] {
                    callWith(name, 11, 0.0, 200.0);
                });
                then(std::string(name) + " is refused as denied, before any read", [&] {
                    require(answer.contains(inspect::code::denied));
                    require(!answer.contains("ping"));
                });
            }
            then("no duration is measured for a page that was refused", [&] {
                callWith("ping", 3, 1);
                for (const auto& info : browser.app.registeredFunctions())
                    requireEqual(info.timedCalls, std::uint64_t{0});
            });
        });
        given("a loopback page that is not proven, with trustLoopbackClients", [&] {
            freshNetwork();
            ViewSocket::peerIsLoopback = true;
            AdmittedBrowser<InspectorApp> browser{true, false};
            browser.app.cppFunction("ping", [](int) {
                return std::string{"pong"};
            });
            const auto answer = answerOf([] {
                callWith("webfront.inspector.functions", 11, 0.0, 200.0);
            });
            then("it is admitted, and is told that it is a local client that is not an embedded view", [&] {
                require(answer.contains("ping"));
                const auto browsers = answerOf([] {
                    callWith("webfront.inspector.browsers", 13, 0.0, 200.0);
                });
                using namespace inspect::browser_flag;
                require(browsers.contains(encodedValue(std::tuple{0.0, double{localClient | started | caller}, std::string{"/"}}).substr(2)));
            });
        });
        given("a developer application that is not bootstrapped, asked for the relay of the bridge", [&] {
            freshNetwork();
            webfront::test::TemporaryDirectory project;
            InspectorApp                       app(project.path(), WebFrontConfig{.port = "0"});
            then("it has none of its functions", [&] {
                for (const auto* name : {"webfront.inspector.bridgeSource", "webfront.inspector.publishBridge", "webfront.inspector.bridge"})
                    require(!app.hasCppFunction(name));
            });
        });
        given("a developer application that is not bootstrapped", [&] {
            freshNetwork();
            webfront::test::TemporaryDirectory project;
            InspectorApp                       app(project.path(), WebFrontConfig{.port = "0"});
            then("it has none of these functions, and counts no call to JavaScript", [&] {
                for (const auto* name : {"webfront.inspector.functions", "webfront.inspector.jsFunctions", "webfront.inspector.browsers"})
                    require(!app.hasCppFunction(name));
                require(app.jsCallCounter("draw") == nullptr);
            });
        });
      }
};
