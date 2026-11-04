// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief A connection proves it comes from the view of an embedded frontend by the secret of the launch (#453, ADR-020).
#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/WebFront.hpp>
#include <webfront/tooling/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace webfront;

namespace webfront::testing {
template <typename Application>
struct LaunchSecret {
    static const std::string& of(const Application& app) { return app.viewSecret; }
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
using ViewApp = BasicWF<ViewOneBrowserNet, fs::DefaultFS>;

struct ViewVerdict {
    bool embedded = false;
    bool local    = false;
};

// Connects one browser whose handshake carries `proof` after its two header bytes, and reports what the UI says.
ViewVerdict connect(ViewApp& app, const std::string& proof) {
    ViewVerdict verdict;
    bool    started = false;
    app.onUIStarted([&](ViewApp::UI ui) {
        verdict.embedded = ui.isEmbeddedView();
        verdict.local    = ui.isLocalClient();
        started          = true;
    });
    ViewSocket::receive(std::as_bytes(std::span(viewUpgradeRequest)));
    msg::Handshake         handshake;
    std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
    for (const char c : proof)
        payload.push_back(static_cast<std::byte>(c));
    ViewSocket::receive(testing::clientFrame(payload));
    REQUIRE(started);
    return verdict;
}

void freshNetwork() {
    ViewSocket::reset();
    ViewSocket::peerIsLoopback  = false;
    ViewOneBrowserNet::accepted = false;
}
}  // namespace

SCENARIO("A connection is associated with the view of an embedded frontend only by the secret of the launch", "[view-association]") {
    freshNetwork();
    ViewApp app{WebFrontConfig{.port = "0"}};
    const std::string secret = testing::LaunchSecret<ViewApp>::of(app);

    GIVEN("the secret of the launch") {
        THEN("it is 256 random bits in lowercase hexadecimal") {
            REQUIRE(secret.size() == 64);
            REQUIRE(std::ranges::all_of(secret, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)); }));
        }
        THEN("another application has another one") {
            ViewApp other{WebFrontConfig{.port = "0"}};
            REQUIRE(testing::LaunchSecret<ViewApp>::of(other) != secret);
        }
    }
    WHEN("the handshake presents the secret") {
        const auto verdict = connect(app, secret);
        THEN("the connection is the embedded view and a local client") {
            REQUIRE(verdict.embedded);
            REQUIRE(verdict.local);
        }
    }
    WHEN("the handshake presents a wrong secret of the right length") {
        auto wrong = secret;
        wrong.back() = wrong.back() == '0' ? '1' : '0';
        const auto verdict = connect(app, wrong);
        THEN("the connection is neither") {
            REQUIRE_FALSE(verdict.embedded);
            REQUIRE_FALSE(verdict.local);
        }
    }
    WHEN("the handshake presents a prefix of the secret") {
        THEN("it is not accepted") {
            REQUIRE_FALSE(connect(app, secret.substr(0, 63)).embedded);
        }
    }
    WHEN("the handshake presents no secret") {
        const auto verdict = connect(app, {});
        THEN("the connection is neither, whatever its address") {
            REQUIRE_FALSE(verdict.embedded);
            REQUIRE_FALSE(verdict.local);
        }
    }
    WHEN("the handshake carries the secret of an earlier launch") {
        std::string earlier;
        {
            ViewApp previous{WebFrontConfig{.port = "0"}};
            earlier = testing::LaunchSecret<ViewApp>::of(previous);
        }
        const auto verdict = connect(app, earlier);
        THEN("it is rejected: the secret does not survive a restart") {
            REQUIRE_FALSE(verdict.embedded);
        }
    }
    WHEN("the handshake carries the secret padded with extra bytes") {
        REQUIRE_FALSE(connect(app, secret + "00").embedded);
    }
    WHEN("the handshake carries a proof longer than a secret") {
        REQUIRE_FALSE(connect(app, std::string(100, 'a')).embedded);
    }
    WHEN("a second handshake without the secret follows the one that presented it") {
        std::optional<ViewApp::UI> browser;
        app.onUIStarted([&](ViewApp::UI ui) { browser = ui; });
        ViewSocket::receive(std::as_bytes(std::span(viewUpgradeRequest)));
        msg::Handshake         handshake;
        std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
        for (const char c : secret)
            payload.push_back(static_cast<std::byte>(c));
        ViewSocket::receive(testing::clientFrame(payload));
        REQUIRE(browser);
        REQUIRE(browser->isEmbeddedView());
        ViewSocket::receive(testing::clientFrame(std::vector<std::byte>(handshake.header().begin(), handshake.header().end())));
        THEN("the connection stays the embedded view") {
            REQUIRE(browser->isEmbeddedView());
            REQUIRE(browser->isLocalClient());
        }
    }
}

SCENARIO("The script that hands the secret to a view refuses what it cannot write safely", "[view-association]") {
    const std::string secret(64, 'a');
    GIVEN("a hexadecimal secret and a plain origin") {
        const auto script = detail::viewSecretScript(secret, "http://127.0.0.1:9002");
        THEN("the script names the origin and the secret") {
            REQUIRE(script.contains("location.origin === 'http://127.0.0.1:9002'"));
            REQUIRE(script.contains(secret));
        }
    }
    GIVEN("an origin or a secret that would need escaping") {
        THEN("there is no script, hence no association") {
            REQUIRE(detail::viewSecretScript(secret, "http://x'; alert(1); '").empty());
            REQUIRE(detail::viewSecretScript(secret, "http://x\\").empty());
            REQUIRE(detail::viewSecretScript(secret, "http://x\n").empty());
            REQUIRE(detail::viewSecretScript("a'b", "http://127.0.0.1:9002").empty());
            REQUIRE(detail::viewSecretScript({}, "http://127.0.0.1:9002").empty());
        }
    }
}

SCENARIO("The origin given to a view is written as the browser writes location.origin", "[view-association]") {
    THEN("the default port 80 is dropped and the host is lowercase") {
        REQUIRE(detail::browserOrigin("127.0.0.1", "80") == "http://127.0.0.1");
        REQUIRE(detail::browserOrigin("LocalHost", "9002") == "http://localhost:9002");
        REQUIRE(detail::browserOrigin("127.0.0.1", "8080") == "http://127.0.0.1:8080");
    }
}

SCENARIO("An IPv4 loopback seen through a dual-stack socket is a loopback peer", "[view-association]") {
    THEN("the v4-mapped loopback is, another mapped address and a public one are not") {
        REQUIRE(detail::isLoopbackAddress(stdnet::ip::address(stdnet::ip::address_v4::loopback())));
        REQUIRE(detail::isLoopbackAddress(stdnet::ip::address(stdnet::ip::address_v6::loopback())));
        const auto mapped = [](unsigned char first) {
            stdnet::ip::address_v6::bytes_type bytes{};
            bytes[10] = bytes[11] = 0xFF;
            bytes[12]             = first;
            bytes[15]             = 1;
            return stdnet::ip::address(stdnet::ip::address_v6(bytes));
        };
        REQUIRE(detail::isLoopbackAddress(mapped(127)));
        REQUIRE_FALSE(detail::isLoopbackAddress(mapped(10)));
    }
}

SCENARIO("A loopback peer is a local client only when the application trusts loopback clients", "[view-association]") {
    freshNetwork();
    ViewSocket::peerIsLoopback = true;

    GIVEN("the default configuration") {
        ViewApp app{WebFrontConfig{.port = "0"}};
        const auto verdict = connect(app, {});
        THEN("a loopback peer without the secret is not local") {
            REQUIRE_FALSE(verdict.embedded);
            REQUIRE_FALSE(verdict.local);
        }
    }
    GIVEN("trustLoopbackClients") {
        ViewApp app{WebFrontConfig{.port = "0", .trustLoopbackClients = true}};
        const auto verdict = connect(app, {});
        THEN("a loopback peer is local though it is not the embedded view") {
            REQUIRE_FALSE(verdict.embedded);
            REQUIRE(verdict.local);
        }
    }
    GIVEN("trustLoopbackClients and a peer that is not the loopback") {
        ViewSocket::peerIsLoopback = false;
        ViewApp app{WebFrontConfig{.port = "0", .trustLoopbackClients = true}};
        const auto verdict = connect(app, {});
        THEN("the connection is not local") {
            REQUIRE_FALSE(verdict.local);
        }
    }
}

SCENARIO("The handshake's secret never reaches the diagnostics", "[view-association]") {
    freshNetwork();
    ViewApp                    app{WebFrontConfig{.port = "0"}};
    std::vector<std::string> records;
    log::setLogLevel(log::Debug);
    const auto sink = log::addSinks([&records](std::string_view text) { records.emplace_back(text); });

    WHEN("a handshake carrying a recognizable proof is received") {
        connect(app, std::string(64, 'Q'));
        log::removeSinks(sink);
        THEN("the message is logged, but no record contains the proof in text or as a hexadecimal dump") {
            REQUIRE(std::ranges::any_of(records, [](const std::string& record) { return record.contains("onMessage(binary)"); }));
            for (const auto& record : records) {
                REQUIRE_FALSE(record.contains("QQQQ"));
                REQUIRE_FALSE(record.contains("51 51 51"));
                REQUIRE_FALSE(record.contains("515151"));
            }
        }
    }
    log::removeSinks(sink);
}
