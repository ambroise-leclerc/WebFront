#include "Mocks.hpp"
#include "Bdd.hpp"
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

using namespace webfront::bdd;

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

const Scenario contentPolicyCompares{"Content policy compares complete HTTP origins", [] {
    const detail::BoundContentPolicy policy{"http://127.0.0.1:9002/index.html"};
    given("the default policy", [&] {
        then("same-origin URLs and native clients are allowed", [&] {
            require(policy.allowsBridge("http://127.0.0.1:9002"));
            require(policy.allowsBridge(std::nullopt));
            require(policy.allowsNavigation("http://127.0.0.1:9002/next?x=1#fragment"));
        });
        then("a Blob of the application's origin (a generated download) is allowed, one of another origin is not", [&] {
            require(policy.allowsNavigation("blob:http://127.0.0.1:9002/0b1f2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"));
            require(policy.allowsNavigation("BLOB:http://127.0.0.1:9002/id"));
            for (const auto* url : {"blob:https://example.com/id", "blob:null/id", "blob:", "blob:http://127.0.0.1:9002@evil/id", "blob:blob:http://127.0.0.1:9002/id"}) {
                auto scope = info(url);
                require(!policy.allowsNavigation(url));
            }
        });
        then("other origins and ambiguous syntax fail closed", [&] {
            for (const auto* origin : invalidBridgeOrigins) {
                auto scope = info(origin);
                require(!policy.allowsBridge(origin));
            }
            for (const auto* url : {"https://example.com/",
                                    "http://127.0.0.1:9002@evil/",
                                    "javascript:alert(1)",
                                    "data:text/html,hi",
                                    "file:///tmp/index.html",
                                    "about:blank",
                                    "http://127.0.0.1:9002\\@evil/"}) {
                auto scope = info(url);
                require(!policy.allowsNavigation(url));
            }
        });
    });
    given("explicit allow-lists and mandatory Origin", [&] {
        const ContentPolicy              options{.bridgeAllowedOrigins     = {"HTTPS://UI.EXAMPLE:443"},
                                                 .navigationAllowedOrigins = {"https://docs.example"},
                                                 .allowMissingOrigin       = false};
        const detail::BoundContentPolicy configured{"http://127.0.0.1:9002", options};
        require(configured.allowsBridge("https://ui.example"));
        require(!configured.allowsBridge(std::nullopt));
        require(!configured.allowsBridge("https://docs.example"));
        require(configured.allowsNavigation("https://docs.example/help"));
        require(!configured.allowsNavigation("https://ui.example/"));
        requireThrows<std::invalid_argument>([&] { (detail::BoundContentPolicy{"http://127.0.0.1:9002", {.bridgeAllowedOrigins = {"*"}}}); });
        requireThrows<std::invalid_argument>([&] { (detail::BoundContentPolicy{"http://127.0.0.1:9002", {.navigationAllowedOrigins = {"https://example.com/path"}}}); });
    });
    given("the explicit permissive HTTP(S) development options", [&] {
        const detail::BoundContentPolicy configured{
            "http://localhost:80",
            {.allowAnyBridgeOrigin = true, .allowAnyNavigation = true}
        };
        require(configured.allowsBridge("https://other.example"));
        require(configured.allowsNavigation("http://other.example/path"));
        require(!configured.allowsBridge("null"));
        require(!configured.allowsNavigation("javascript:alert(1)"));
    });
    given("default ports, case and bracketed IPv6", [&] {
        require(detail::BoundContentPolicy{"http://LOCALHOST:80"}.allowsBridge("http://localhost"));
        require(detail::BoundContentPolicy{"https://[::1]:443"}.allowsBridge("https://[::1]"));
        require(!(detail::BoundContentPolicy{"http://[::1]:9002"}.allowsBridge("http://[::1]:9003")));
    });
}};

const Scenario contentPolicyControls{"Content policy controls popup delegation", [] {
    std::vector<std::string> opened;
    auto                     launch = [&](const std::string& url) {
        opened.push_back(url);
    };
    given("the default policy", [&] {
        detail::BoundContentPolicy{}.handlePopup("https://example.com/help", true, launch);
        require(opened.empty());
    });
    given("system browser popups are explicitly enabled", [&] {
        const detail::BoundContentPolicy policy{"http://127.0.0.1:9002", {.popups = PopupPolicy::systemBrowser}};
        policy.handlePopup("https://example.com/help", true, launch);
        policy.handlePopup("file:///etc/passwd", true, launch);
        policy.handlePopup("javascript:alert(1)", true, launch);
        require(opened == std::vector<std::string>{"https://example.com/help"});
        when("a script requests repeated popups without a user gesture", [&] {
            opened.clear();
            for (int count = 0; count < 100; ++count)
                policy.handlePopup("https://example.com/help", false, launch);
            then("no URL reaches the external launcher", [&] {
                require(opened.empty());
            });
        });
    });
}};

const Scenario webSocketUpgrades{"WebSocket upgrades authorize Origin before returning 101", [] {
    using namespace webfront::http;
    using Net    = networking::NetworkingMock;
    auto upgrade = [](const std::string& headers, ContentPolicy policy = {}) {
        const std::string input = "GET / HTTP/1.1\r\nHost: attacker.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                  + headers + "\r\n";
        Request           request;
        require(request.parseSomeData(input.begin(), input.end()));
        RequestHandler<Net, MockFileSystem<>> handler{
            ".",
            detail::BoundContentPolicy{"http://127.0.0.1:9002/", std::move(policy)}
        };
        return handler.handleRequest(request);
    };
    then("the configured application origin and absent Origin are accepted independently of Host", [&] {
        require(upgrade("Origin: http://127.0.0.1:9002\r\n").statusCode == Response::switchingProtocols);
        require(upgrade("").statusCode == Response::switchingProtocols);
    });
    then("cross-origin, opaque, malformed and duplicate Origin receive a deterministic 403", [&] {
        for (const auto* headers : {"Origin: https://attacker.example\r\n",
                                    "Origin: null\r\n",
                                    "Origin: http://127.0.0.1:9002/path\r\n",
                                    "Origin: http://127.0.0.1:9002\r\norigin: https://attacker.example\r\n",
                                    "Origin: http://127.0.0.1:9002\r\nOrigin: http://127.0.0.1:9002\r\n"}) {
            const auto response = upgrade(headers);
            require(response.statusCode == Response::forbidden);
            require(!response.getHeaderValue("Sec-WebSocket-Accept"));
            require(!response.getHeaderValue("Upgrade"));
            require(response.content.contains("WebSocket Origin rejected by content policy"));
            require(response.content.contains("bridgeAllowedOrigins"));
            require(response.getHeaderValue("Content-Type") == "text/plain; charset=utf-8");
            require(response.getHeaderValue("Content-Length") == std::to_string(response.content.size()));
        }
    });
    then("the allow-list and native-client opt-out reach the handshake", [&] {
        require(upgrade("Origin: https://ui.example\r\n", {.bridgeAllowedOrigins = {"https://ui.example"}}).statusCode == Response::switchingProtocols);
        require(upgrade("", {.allowMissingOrigin = false}).statusCode == Response::forbidden);
    });
    then("a refusal logs an actionable diagnostic for alternate hosts and wildcard binds", [&] {
        WarningCapture capture;
        require(upgrade("Origin: http://192.168.1.2:9002\r\n").statusCode == Response::forbidden);
        require(!capture.messages.empty());
        require(capture.messages.back().contains("bridgeAllowedOrigins"));
        require(capture.messages.back().contains("wildcard binds authorize loopback only"));
    });
}};
