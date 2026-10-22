#include "Mocks.hpp"
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <webfront/ContentPolicy.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/tooling/Logger.hpp>

#include <array>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace webfront;

namespace {
constexpr std::array invalidBridgeOrigins{"null",
                                          "",
                                          "http://localhost:9002",
                                          "https://127.0.0.1:9002",
                                          "http://127.0.0.1:9003",
                                          "http://127.0.0.1:9002.evil",
                                          "http://127.0.0.1:9002@evil",
                                          "http://127.0.0.1:9002/",
                                          "http://127.0.0.1:9002 http://evil",
                                          "http://127.0.0.1:9002, http://evil",
                                          "http://127.0.0.1:65536",
                                          "http://[::1",
                                          "http://[]",
                                          "http://[::g]",
                                          "http://[abcd]",
                                          "http://[::1]suffix",
                                          "http://[::1]:",
                                          "http://",
                                          "http://:9002",
                                          "http://localhost:",
                                          "http://localhost:123456",
                                          "http://localhost:wrong"};

struct WarningCapture {
    using Sink = decltype(log::addSinks(std::function<void(std::string_view)>{}));
    std::vector<std::string> messages;
    bool                     wasEnabled = log::is(log::Warn);
    Sink                     sink       = log::addSinks([this](std::string_view text) {
        messages.emplace_back(text);
    });
    WarningCapture() {
        log::set(log::Warn, true);
    }
    WarningCapture(const WarningCapture&)            = delete;
    WarningCapture& operator=(const WarningCapture&) = delete;
    WarningCapture(WarningCapture&&)                 = delete;
    WarningCapture& operator=(WarningCapture&&)      = delete;
    ~WarningCapture() {
        log::removeSinks(sink);
        log::set(log::Warn, wasEnabled);
    }
};
}  // namespace

SCENARIO("Content policy compares complete HTTP origins") {
    const detail::BoundContentPolicy policy{"http://127.0.0.1:9002/index.html"};
    GIVEN("the default policy") {
        THEN("same-origin URLs and native clients are allowed") {
            REQUIRE(policy.allowsBridge("http://127.0.0.1:9002"));
            REQUIRE(policy.allowsBridge(std::nullopt));
            REQUIRE(policy.allowsNavigation("http://127.0.0.1:9002/next?x=1#fragment"));
        }
        THEN("other origins and ambiguous syntax fail closed") {
            for (const auto* origin : invalidBridgeOrigins) {
                INFO(origin);
                REQUIRE_FALSE(policy.allowsBridge(origin));
            }
            for (const auto* url : {"https://example.com/",
                                    "http://127.0.0.1:9002@evil/",
                                    "javascript:alert(1)",
                                    "data:text/html,hi",
                                    "file:///tmp/index.html",
                                    "about:blank",
                                    "http://127.0.0.1:9002\\@evil/"}) {
                INFO(url);
                REQUIRE_FALSE(policy.allowsNavigation(url));
            }
        }
    }
    GIVEN("explicit allow-lists and mandatory Origin") {
        const ContentPolicy              options{.bridgeAllowedOrigins     = {"HTTPS://UI.EXAMPLE:443"},
                                                 .navigationAllowedOrigins = {"https://docs.example"},
                                                 .allowMissingOrigin       = false};
        const detail::BoundContentPolicy configured{"http://127.0.0.1:9002", options};
        REQUIRE(configured.allowsBridge("https://ui.example"));
        REQUIRE_FALSE(configured.allowsBridge(std::nullopt));
        REQUIRE_FALSE(configured.allowsBridge("https://docs.example"));
        REQUIRE(configured.allowsNavigation("https://docs.example/help"));
        REQUIRE_FALSE(configured.allowsNavigation("https://ui.example/"));
        REQUIRE_THROWS_AS((detail::BoundContentPolicy{"http://127.0.0.1:9002", {.bridgeAllowedOrigins = {"*"}}}), std::invalid_argument);
        REQUIRE_THROWS_AS((detail::BoundContentPolicy{"http://127.0.0.1:9002", {.navigationAllowedOrigins = {"https://example.com/path"}}}),
                          std::invalid_argument);
    }
    GIVEN("the explicit permissive HTTP(S) development options") {
        const detail::BoundContentPolicy configured{
            "http://localhost:80",
            {.allowAnyBridgeOrigin = true, .allowAnyNavigation = true}
        };
        REQUIRE(configured.allowsBridge("https://other.example"));
        REQUIRE(configured.allowsNavigation("http://other.example/path"));
        REQUIRE_FALSE(configured.allowsBridge("null"));
        REQUIRE_FALSE(configured.allowsNavigation("javascript:alert(1)"));
    }
    GIVEN("default ports, case and bracketed IPv6") {
        REQUIRE(detail::BoundContentPolicy{"http://LOCALHOST:80"}.allowsBridge("http://localhost"));
        REQUIRE(detail::BoundContentPolicy{"https://[::1]:443"}.allowsBridge("https://[::1]"));
        REQUIRE_FALSE(detail::BoundContentPolicy{"http://[::1]:9002"}.allowsBridge("http://[::1]:9003"));
    }
}

SCENARIO("Content policy controls popup delegation") {
    std::vector<std::string> opened;
    auto                     launch = [&](const std::string& url) {
        opened.push_back(url);
    };
    GIVEN("the default policy") {
        detail::BoundContentPolicy{}.handlePopup("https://example.com/help", true, launch);
        REQUIRE(opened.empty());
    }
    GIVEN("system browser popups are explicitly enabled") {
        const detail::BoundContentPolicy policy{"http://127.0.0.1:9002", {.popups = PopupPolicy::systemBrowser}};
        policy.handlePopup("https://example.com/help", true, launch);
        policy.handlePopup("file:///etc/passwd", true, launch);
        policy.handlePopup("javascript:alert(1)", true, launch);
        REQUIRE(opened == std::vector<std::string>{"https://example.com/help"});
        WHEN("a script requests repeated popups without a user gesture") {
            opened.clear();
            for (int count = 0; count < 100; ++count)
                policy.handlePopup("https://example.com/help", false, launch);
            THEN("no URL reaches the external launcher") {
                REQUIRE(opened.empty());
            }
        }
    }
}

SCENARIO("WebSocket upgrades authorize Origin before returning 101") {
    using namespace webfront::http;
    using Net    = networking::NetworkingMock;
    auto upgrade = [](const std::string& headers, ContentPolicy policy = {}) {
        const std::string input = "GET / HTTP/1.1\r\nHost: attacker.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                  + headers + "\r\n";
        Request           request;
        REQUIRE(request.parseSomeData(input.begin(), input.end()));
        RequestHandler<Net, MockFileSystem<>> handler{
            ".",
            detail::BoundContentPolicy{"http://127.0.0.1:9002/", std::move(policy)}
        };
        return handler.handleRequest(request);
    };
    THEN("the configured application origin and absent Origin are accepted independently of Host") {
        REQUIRE(upgrade("Origin: http://127.0.0.1:9002\r\n").statusCode == Response::switchingProtocols);
        REQUIRE(upgrade("").statusCode == Response::switchingProtocols);
    }
    THEN("cross-origin, opaque, malformed and duplicate Origin receive a deterministic 403") {
        for (const auto* headers : {"Origin: https://attacker.example\r\n",
                                    "Origin: null\r\n",
                                    "Origin: http://127.0.0.1:9002/path\r\n",
                                    "Origin: http://127.0.0.1:9002\r\norigin: https://attacker.example\r\n",
                                    "Origin: http://127.0.0.1:9002\r\nOrigin: http://127.0.0.1:9002\r\n"}) {
            const auto response = upgrade(headers);
            REQUIRE(response.statusCode == Response::forbidden);
            REQUIRE_FALSE(response.getHeaderValue("Sec-WebSocket-Accept"));
            REQUIRE_FALSE(response.getHeaderValue("Upgrade"));
            REQUIRE(response.content.contains("WebSocket Origin rejected by content policy"));
            REQUIRE(response.content.contains("bridgeAllowedOrigins"));
            REQUIRE(response.getHeaderValue("Content-Type") == "text/plain; charset=utf-8");
            REQUIRE(response.getHeaderValue("Content-Length") == std::to_string(response.content.size()));
        }
    }
    THEN("the allow-list and native-client opt-out reach the handshake") {
        REQUIRE(upgrade("Origin: https://ui.example\r\n", {.bridgeAllowedOrigins = {"https://ui.example"}}).statusCode == Response::switchingProtocols);
        REQUIRE(upgrade("", {.allowMissingOrigin = false}).statusCode == Response::forbidden);
    }
    THEN("a refusal logs an actionable diagnostic for alternate hosts and wildcard binds") {
        WarningCapture capture;
        REQUIRE(upgrade("Origin: http://192.168.1.2:9002\r\n").statusCode == Response::forbidden);
        REQUIRE_FALSE(capture.messages.empty());
        REQUIRE(capture.messages.back().contains("bridgeAllowedOrigins"));
        REQUIRE(capture.messages.back().contains("wildcard binds authorize loopback only"));
    }
}
