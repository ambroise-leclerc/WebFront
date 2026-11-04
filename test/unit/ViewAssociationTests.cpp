// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief A connection proves it comes from the view of an embedded frontend by the secret of the launch (#453, ADR-020).
#include "InjectableNetworking.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/WebFront.hpp>

#include <algorithm>
#include <cctype>
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

using Socket = testing::InjectableSocket;
using App    = BasicWF<OneBrowserNet, fs::DefaultFS>;

struct Verdict {
    bool embedded = false;
    bool local    = false;
};

// Connects one browser whose handshake carries `proof` after its two header bytes, and reports what the UI says.
Verdict connect(App& app, const std::string& proof) {
    Verdict verdict;
    bool    started = false;
    app.onUIStarted([&](App::UI ui) {
        verdict.embedded = ui.isEmbeddedView();
        verdict.local    = ui.isLocalClient();
        started          = true;
    });
    Socket::receive(std::as_bytes(std::span(upgradeRequest)));
    msg::Handshake         handshake;
    std::vector<std::byte> payload(handshake.header().begin(), handshake.header().end());
    for (const char c : proof)
        payload.push_back(static_cast<std::byte>(c));
    Socket::receive(testing::clientFrame(payload));
    REQUIRE(started);
    return verdict;
}

void freshNetwork() {
    Socket::reset();
    Socket::peerIsLoopback  = false;
    OneBrowserNet::accepted = false;
}
}  // namespace

SCENARIO("A connection is associated with the view of an embedded frontend only by the secret of the launch", "[view-association]") {
    freshNetwork();
    App app{WebFrontConfig{.port = "0"}};
    const std::string secret = testing::LaunchSecret<App>::of(app);

    GIVEN("the secret of the launch") {
        THEN("it is 256 random bits in lowercase hexadecimal") {
            REQUIRE(secret.size() == 64);
            REQUIRE(std::ranges::all_of(secret, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(c)); }));
        }
        THEN("another application has another one") {
            App other{WebFrontConfig{.port = "0"}};
            REQUIRE(testing::LaunchSecret<App>::of(other) != secret);
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
    WHEN("the handshake presents a prefix of the secret or the secret with more bytes") {
        THEN("neither is accepted") {
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
            App previous{WebFrontConfig{.port = "0"}};
            earlier = testing::LaunchSecret<App>::of(previous);
        }
        const auto verdict = connect(app, earlier);
        THEN("it is rejected: the secret does not survive a restart") {
            REQUIRE_FALSE(verdict.embedded);
        }
    }
    WHEN("the handshake carries the secret padded with extra bytes") {
        REQUIRE_FALSE(connect(app, secret + "00").embedded);
    }
}

SCENARIO("A loopback peer is a local client only when the application trusts loopback clients", "[view-association]") {
    freshNetwork();
    Socket::peerIsLoopback = true;

    GIVEN("the default configuration") {
        App app{WebFrontConfig{.port = "0"}};
        const auto verdict = connect(app, {});
        THEN("a loopback peer without the secret is not local") {
            REQUIRE_FALSE(verdict.embedded);
            REQUIRE_FALSE(verdict.local);
        }
    }
    GIVEN("trustLoopbackClients") {
        App app{WebFrontConfig{.port = "0", .trustLoopbackClients = true}};
        const auto verdict = connect(app, {});
        THEN("a loopback peer is local though it is not the embedded view") {
            REQUIRE_FALSE(verdict.embedded);
            REQUIRE(verdict.local);
        }
    }
    GIVEN("trustLoopbackClients and a peer that is not the loopback") {
        Socket::peerIsLoopback = false;
        App app{WebFrontConfig{.port = "0", .trustLoopbackClients = true}};
        const auto verdict = connect(app, {});
        THEN("the connection is not local") {
            REQUIRE_FALSE(verdict.local);
        }
    }
}
