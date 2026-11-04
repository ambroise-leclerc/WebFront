// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The binding of HTTP reads to the browser client of a connection: token, cookie, transport, neutralized content (#469, ADR-020).
#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/ReadBinding.hpp>
#include <webfront/WebFront.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/tooling/Logger.hpp>

#include <atomic>
#include <chrono>
#include <list>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace webfront;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view prefix = "/webfront/services/files/";

std::optional<std::string> header(const http::RouteReply& reply, std::string_view name) {
    for (const auto& [key, value] : reply.headers)
        if (detail::lowerAscii(key) == detail::lowerAscii(name))
            return value;
    return std::nullopt;
}

std::vector<std::string> headers(const http::RouteReply& reply, std::string_view name) {
    std::vector<std::string> values;
    for (const auto& [key, value] : reply.headers)
        if (detail::lowerAscii(key) == detail::lowerAscii(name))
            values.push_back(value);
    return values;
}

/// Parses a request and calls a route handler with it, the way the server does.
http::RouteReply call(const http::RouteHandler& handler, std::string path, std::string cookies = {}, std::string extra = {}) {
    std::string raw = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!cookies.empty())
        raw += "Cookie: " + cookies + "\r\n";
    raw += extra + "\r\n";
    http::Request request;
    request.parseSomeData(raw.begin(), raw.end());
    REQUIRE(request.completed());
    return handler(http::RouteRequest{request, path, {}});
}

/// "name=value" of a Set-Cookie header.
std::string cookiePair(const std::string& setCookie) {
    return setCookie.substr(0, setCookie.find(';'));
}

struct Fixture {
    using Clock = std::chrono::steady_clock;
    Clock::time_point           time = Clock::now();
    ReadBinding                 binding;
    http::RouteHandler          exchange = binding.exchangeHandler();
    http::RouteHandler          read;
    std::string                 lastRest;
    std::any                    lastContext;

    explicit Fixture(RemoteFileTransport transport = RemoteFileTransport::refused)
        : binding(transport, [this] { return time; }) {
        read = binding.readHandler(std::string(prefix), [this](const http::RouteRequest&, const ReadGrant& grant) {
            lastRest    = std::string(grant.rest);
            lastContext = grant.context;
            http::RouteReply reply;
            reply.headers.emplace_back("Content-Type", "text/plain");
            reply.contentLength = 2;
            return reply;
        });
    }

    /// Opens a connection and performs the exchange with `cookies`; returns the reply (cookies to keep in Set-Cookie).
    http::RouteReply bind(ReadBinding::ConnectionKey key, bool local, const std::string& cookies = {}) {
        const auto secret = binding.open(key, local);
        return call(exchange, std::string(ReadBinding::bindPath) + secret, cookies);
    }

    std::string url(ReadBinding::ConnectionKey key, std::string_view path = "a.txt", ReadOptions options = {}) {
        return binding.grant(key, prefix, path, std::move(options));
    }
};

}  // namespace

SCENARIO("A read needs the token and the cookie of the client of the connection that owns it", "[read-binding]") {
    Fixture fixture;
    GIVEN("a local connection that exchanged its secret for a cookie") {
        const auto bound = fixture.bind(1, true);
        const auto cookies = headers(bound, "Set-Cookie");
        REQUIRE(bound.status == http::Response::ok);
        REQUIRE(cookies.size() == 1);
        const auto cookie = cookiePair(cookies.front());
        const auto url    = fixture.url(1, "dir/a b.txt");

        THEN("the cookie is named after its client and restricted") {
            REQUIRE(cookie.starts_with("wf-client-"));
            REQUIRE(cookies.front().contains("; Path=/webfront/services/"));
            REQUIRE(cookies.front().contains("; HttpOnly"));
            REQUIRE(cookies.front().contains("; SameSite=Strict"));
            REQUIRE_FALSE(cookies.front().contains("Secure"));
        }
        THEN("the URL is under the prefix, followed by the token and the encoded path") {
            REQUIRE(url.starts_with(prefix));
            REQUIRE(url.ends_with("/dir/a%20b.txt"));
        }
        WHEN("the owner's client presents the URL with its cookie") {
            const auto reply = call(fixture.read, url, cookie);
            THEN("the handler runs with the decoded rest of the path") {
                REQUIRE(reply.status == http::Response::ok);
                REQUIRE(fixture.lastRest == "dir/a%20b.txt");  // the test passes the raw URL as path: the server decodes it
            }
        }
        WHEN("the URL is presented without cookie") {
            REQUIRE(call(fixture.read, url).status == http::Response::forbidden);
            THEN("the handler did not run") { REQUIRE(fixture.lastRest.empty()); }
        }
        WHEN("the URL is presented with the cookie of another client") {
            const auto other = cookiePair(headers(fixture.bind(2, true), "Set-Cookie").front());
            REQUIRE(other != cookie);
            THEN("it is refused") { REQUIRE(call(fixture.read, url, other).status == http::Response::forbidden); }
        }
        WHEN("the URL is presented with the right cookie name and a wrong value") {
            const auto forged = cookie.substr(0, cookie.size() - 1) + (cookie.back() == '0' ? "1" : "0");
            THEN("it is refused") { REQUIRE(call(fixture.read, url, forged).status == http::Response::forbidden); }
        }
        WHEN("the token is altered or absent") {
            auto altered = url;
            altered[prefix.size() + 20] = altered[prefix.size() + 20] == 'a' ? 'b' : 'a';
            THEN("every refusal looks alike") {
                const auto unknown = call(fixture.read, altered, cookie);
                const auto missing = call(fixture.read, std::string(prefix), cookie);
                const auto garbage = call(fixture.read, std::string(prefix) + "nodot/x", cookie);
                REQUIRE(unknown.status == http::Response::forbidden);
                REQUIRE(missing.status == http::Response::forbidden);
                REQUIRE(garbage.status == http::Response::forbidden);
                REQUIRE(unknown.headers == missing.headers);
                REQUIRE(unknown.headers == garbage.headers);
            }
        }
        WHEN("the URL is requested repeatedly, as a media element does for its byte ranges") {
            THEN("the token stays valid") {
                for (int range = 0; range < 5; ++range)
                    REQUIRE(call(fixture.read, url, cookie).status == http::Response::ok);
            }
        }
        WHEN("the owning connection closes") {
            fixture.binding.close(1);
            THEN("the URL no longer works, cookie or not") { REQUIRE(call(fixture.read, url, cookie).status == http::Response::forbidden); }
        }
        WHEN("the grant is revoked") {
            fixture.binding.revoke(url);
            THEN("the URL no longer works") { REQUIRE(call(fixture.read, url, cookie).status == http::Response::forbidden); }
        }
        WHEN("the server restarts: another binding sees the old cookie and URL") {
            Fixture restarted;
            THEN("neither is known") { REQUIRE(call(restarted.read, url, cookie).status == http::Response::forbidden); }
            AND_THEN("the old cookie is erased at the next exchange") {
                const auto again = restarted.bind(1, true, cookie);
                bool erased = false;
                for (const auto& line : headers(again, "Set-Cookie"))
                    erased = erased || (line.starts_with(cookie.substr(0, cookie.find('=')) + "=;") && line.contains("Max-Age=0"));
                REQUIRE(erased);
            }
        }
    }
}

SCENARIO("The connections of one browser profile belong to one client", "[read-binding]") {
    Fixture fixture;
    GIVEN("a first tab that exchanged and a second tab that presents the cookie it already holds") {
        const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
        const auto second = fixture.bind(2, true, cookie);
        THEN("the second tab creates no cookie") { REQUIRE(headers(second, "Set-Cookie").empty()); }
        const auto url1 = fixture.url(1);
        const auto url2 = fixture.url(2);
        THEN("each tab's URL is read with the shared cookie, as the same profile does") {
            REQUIRE(call(fixture.read, url1, cookie).status == http::Response::ok);
            REQUIRE(call(fixture.read, url2, cookie).status == http::Response::ok);
        }
        AND_WHEN("the first tab closes") {
            fixture.binding.close(1);
            THEN("the client lives on with the second") {
                REQUIRE(call(fixture.read, url1, cookie).status == http::Response::forbidden);
                REQUIRE(call(fixture.read, url2, cookie).status == http::Response::ok);
            }
            AND_WHEN("the second closes too") {
                fixture.binding.close(2);
                THEN("the client is forgotten: a third tab with the cookie gets a new client and the old cookie is erased") {
                    const auto third = fixture.bind(3, true, cookie);
                    const auto lines = headers(third, "Set-Cookie");
                    REQUIRE(lines.size() == 2);
                    REQUIRE(lines[0].contains("Max-Age=0"));
                    REQUIRE(cookiePair(lines[1]) != cookie);
                }
            }
        }
    }
    GIVEN("two tabs of a blank profile whose exchanges run at the same time") {
        // Both secrets are issued, and both requests are answered, before the browser stores either cookie.
        const auto secretA = fixture.binding.open(1, true);
        const auto secretB = fixture.binding.open(2, true);
        const auto answerA = call(fixture.exchange, std::string(ReadBinding::bindPath) + secretA);
        const auto answerB = call(fixture.exchange, std::string(ReadBinding::bindPath) + secretB);
        const auto cookieA = cookiePair(headers(answerA, "Set-Cookie").front());
        const auto cookieB = cookiePair(headers(answerB, "Set-Cookie").front());
        THEN("the cookies have different names, so neither overwrites the other") {
            REQUIRE(cookieA.substr(0, cookieA.find('=')) != cookieB.substr(0, cookieB.find('=')));
        }
        THEN("the browser sends both, and both tabs read their URLs") {
            const auto both = cookieA + "; " + cookieB;
            REQUIRE(call(fixture.read, fixture.url(1), both).status == http::Response::ok);
            REQUIRE(call(fixture.read, fixture.url(2), both).status == http::Response::ok);
        }
        THEN("each cookie alone opens its own tab's URL only") {
            REQUIRE(call(fixture.read, fixture.url(1), cookieA).status == http::Response::ok);
            REQUIRE(call(fixture.read, fixture.url(2), cookieA).status == http::Response::forbidden);
        }
    }
}

SCENARIO("The exchange secret is single use and only the right one works", "[read-binding]") {
    Fixture fixture;
    const auto secret = fixture.binding.open(1, true);
    const auto path   = std::string(ReadBinding::bindPath) + secret;
    THEN("a wrong secret is refused and gives no cookie") {
        const auto reply = call(fixture.exchange, std::string(ReadBinding::bindPath) + std::string(64, '0'));
        REQUIRE(reply.status == http::Response::forbidden);
        REQUIRE(headers(reply, "Set-Cookie").empty());
    }
    THEN("the secret works once") {
        REQUIRE(call(fixture.exchange, path).status == http::Response::ok);
        const auto replay = call(fixture.exchange, path);
        REQUIRE(replay.status == http::Response::forbidden);
        REQUIRE(headers(replay, "Set-Cookie").empty());
    }
    THEN("a secret does not survive its connection") {
        fixture.binding.close(1);
        REQUIRE(call(fixture.exchange, path).status == http::Response::forbidden);
    }
    THEN("the check answers only to a request that carries a cookie of a known client") {
        const auto check = std::string(ReadBinding::bindPath) + "check";
        REQUIRE(call(fixture.exchange, check).status == http::Response::forbidden);
        const auto cookie = cookiePair(headers(call(fixture.exchange, path), "Set-Cookie").front());
        REQUIRE(call(fixture.exchange, check, cookie).status == http::Response::ok);
        REQUIRE(call(fixture.exchange, check, "wf-client-0000000000000000=" + std::string(64, 'a')).status == http::Response::forbidden);
    }
}

SCENARIO("The transferable mode makes the token alone a short-lived bearer capability", "[read-binding]") {
    Fixture fixture;
    const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
    THEN("it needs an explicit short lifetime") {
        REQUIRE_THROWS_AS(fixture.url(1, "a", {.transferable = true}), ReadRefused);
        REQUIRE_THROWS_AS(fixture.url(1, "a", {.lifetime = 16min, .transferable = true}), ReadRefused);
        REQUIRE_THROWS_AS(fixture.url(1, "a", {.lifetime = -1s}), ReadRefused);
    }
    GIVEN("a transferable URL") {
        const auto url = fixture.url(1, "a", {.lifetime = 60s, .transferable = true});
        THEN("a client without any cookie reads it") { REQUIRE(call(fixture.read, url).status == http::Response::ok); }
        THEN("it stops at its expiry") {
            fixture.time += 59s;
            REQUIRE(call(fixture.read, url).status == http::Response::ok);
            fixture.time += 2s;
            REQUIRE(call(fixture.read, url).status == http::Response::forbidden);
        }
        THEN("it stops with the connection") {
            fixture.binding.close(1);
            REQUIRE(call(fixture.read, url).status == http::Response::forbidden);
        }
    }
    GIVEN("a bound URL with a lifetime") {
        const auto url = fixture.url(1, "a", {.lifetime = 60s});
        THEN("the cookie is still required, and the lifetime still applies") {
            REQUIRE(call(fixture.read, url).status == http::Response::forbidden);
            REQUIRE(call(fixture.read, url, cookie).status == http::Response::ok);
            fixture.time += 61s;
            REQUIRE(call(fixture.read, url, cookie).status == http::Response::forbidden);
        }
    }
    THEN("the context of the grant reaches the handler") {
        const auto url = fixture.url(1, "a", {.context = std::string("handle-7")});
        REQUIRE(call(fixture.read, url, cookie).status == http::Response::ok);
        REQUIRE(std::any_cast<std::string>(fixture.lastContext) == "handle-7");
    }
}

SCENARIO("A connection that is not the view of an embedded frontend reads only under a declared transport", "[read-binding]") {
    GIVEN("the default declaration") {
        Fixture fixture;
        const auto cookie = cookiePair(headers(fixture.bind(1, false), "Set-Cookie").front());
        THEN("no URL is given to the system browser") { REQUIRE_THROWS_AS(fixture.url(1), ReadRefused); }
        THEN("a connection that is closed has none either") { REQUIRE_THROWS_AS(fixture.url(9), ReadRefused); }
        THEN("the embedded view reads") {
            const auto viewCookie = cookiePair(headers(fixture.bind(2, true), "Set-Cookie").front());
            REQUIRE(call(fixture.read, fixture.url(2), viewCookie).status == http::Response::ok);
        }
    }
    for (const auto transport : {RemoteFileTransport::loopbackAttested, RemoteFileTransport::encryptedByDeployment, RemoteFileTransport::plaintextAccepted}) {
        GIVEN("the declared transport " << static_cast<int>(transport)) {
            Fixture fixture{transport};
            const auto bound  = fixture.bind(1, false);
            const auto cookie = cookiePair(headers(bound, "Set-Cookie").front());
            const auto url    = fixture.url(1);
            THEN("the system browser reads with its cookie, whatever the forwarding headers claim") {
                REQUIRE(call(fixture.read, url, cookie, "X-Forwarded-Proto: https\r\nForwarded: for=127.0.0.1;proto=https\r\n").status == http::Response::ok);
                REQUIRE(call(fixture.read, url).status == http::Response::forbidden);
            }
            THEN("the cookie is Secure only when the deployment declares encryption") {
                REQUIRE(headers(bound, "Set-Cookie").front().contains("; Secure") == (transport == RemoteFileTransport::encryptedByDeployment));
            }
        }
    }
    GIVEN("a request through a local proxy that forges the forwarding headers, with the default declaration") {
        Fixture fixture;
        fixture.bind(1, false);
        THEN("nothing makes the connection local: the headers and the address are never consulted") {
            REQUIRE_THROWS_AS(fixture.url(1), ReadRefused);
        }
    }
}

SCENARIO("A declaration of a loopback server is checked against the address it listens on", "[read-binding]") {
    for (const auto* address : {"127.0.0.1", "127.1.2.3", "::1", "[::1]", "localhost", "LocalHost"})
        THEN("'" << address << "' is a loopback address") { REQUIRE(detail::isLoopbackName(address)); }
    for (const auto* address : {"0.0.0.0", "::", "", "192.168.1.5", "10.0.0.1", "127.0.0", "127.0.0.1.5", "127.a.0.1", "127..0.1", "example.com", "127.0.0.1."})
        THEN("'" << address << "' is not") { REQUIRE_FALSE(detail::isLoopbackName(address)); }
    THEN("loopbackAttested is refused for a server that listens elsewhere, and accepted on the loopback") {
        REQUIRE_THROWS_AS(detail::checkedAddress(WebFrontConfig{.address = "0.0.0.0", .remoteFileTransport = RemoteFileTransport::loopbackAttested}),
                          std::invalid_argument);
        REQUIRE_NOTHROW(detail::checkedAddress(WebFrontConfig{.address = "127.0.0.1", .remoteFileTransport = RemoteFileTransport::loopbackAttested}));
        REQUIRE_NOTHROW(detail::checkedAddress(WebFrontConfig{.address = "0.0.0.0", .remoteFileTransport = RemoteFileTransport::plaintextAccepted}));
    }
}

SCENARIO("Read responses neutralize active content", "[read-binding]") {
    const auto present = [](std::string contentType, std::string rest = "dir/file.bin") {
        http::RouteReply reply;
        if (!contentType.empty())
            reply.headers.emplace_back("Content-Type", std::move(contentType));
        reply.headers.emplace_back("Content-Disposition", "inline");  // a handler cannot choose
        detail::hardenRead(reply, rest);
        return reply;
    };
    GIVEN("any response") {
        const auto reply = present("image/png");
        THEN("it carries the protective headers") {
            REQUIRE(header(reply, "Cache-Control") == "no-store");
            REQUIRE(header(reply, "Referrer-Policy") == "no-referrer");
            REQUIRE(header(reply, "Content-Security-Policy") == "sandbox; default-src 'none'");
            REQUIRE(header(reply, "X-Content-Type-Options") == "nosniff");
            REQUIRE(header(reply, "Cross-Origin-Resource-Policy") == "same-origin");
        }
    }
    GIVEN("passive types") {
        for (const auto* type : {"image/png", "image/jpeg", "image/gif", "image/webp", "audio/mpeg", "video/mp4", "text/plain", "text/plain; charset=utf-8", "VIDEO/WEBM"}) {
            THEN(type << " is shown inline with its type") {
                const auto reply = present(type);
                REQUIRE(header(reply, "Content-Type") == type);
                REQUIRE_FALSE(header(reply, "Content-Disposition"));
            }
        }
    }
    GIVEN("active types") {
        for (const auto* type : {"text/html", "text/html; charset=utf-8", "application/xhtml+xml", "text/xml", "application/xml", "application/atom+xml",
                                 "text/javascript", "application/javascript", "application/ecmascript", "application/wasm", "TEXT/HTML"}) {
            THEN(type << " is a download announced as octet-stream") {
                const auto reply = present(type);
                REQUIRE(header(reply, "Content-Type") == "application/octet-stream");
                REQUIRE(header(reply, "Content-Disposition")->starts_with("attachment; filename=\"file.bin\""));
            }
        }
    }
    GIVEN("an SVG") {
        const auto reply = present("image/svg+xml", "logo.svg");
        THEN("it keeps its type, for <img>, and a navigation downloads it") {
            REQUIRE(header(reply, "Content-Type") == "image/svg+xml");
            REQUIRE(header(reply, "Content-Disposition")->starts_with("attachment; filename=\"logo.svg\""));
        }
    }
    GIVEN("a type outside the closed list, or none") {
        THEN("it is a download") {
            REQUIRE(header(present("application/pdf"), "Content-Disposition")->starts_with("attachment"));
            REQUIRE(header(present("application/pdf"), "Content-Type") == "application/pdf");
            REQUIRE(header(present("text/css"), "Content-Disposition")->starts_with("attachment"));
            REQUIRE(header(present(""), "Content-Type") == "application/octet-stream");
            REQUIRE(header(present(""), "Content-Disposition")->starts_with("attachment"));
        }
    }
    GIVEN("a hostile file name") {
        const auto reply = present("text/html", "a\"b\r\nSet-Cookie: x;\\c/é.html");
        THEN("the header stays one well-formed line") {
            const auto disposition = *header(reply, "Content-Disposition");
            REQUIRE(disposition.find('\r') == std::string::npos);
            REQUIRE(disposition.find('\n') == std::string::npos);
            REQUIRE(disposition.starts_with("attachment; filename=\""));
            REQUIRE(disposition.contains("filename*=UTF-8''"));
            REQUIRE(disposition.contains("%C3%A9.html"));
        }
    }
    GIVEN("a response a handler built with headers of its own") {
        http::RouteReply reply;
        reply.headers.emplace_back("Content-Type", "text/plain");
        reply.headers.emplace_back("cache-control", "public, max-age=3600");
        reply.headers.emplace_back("Content-Security-Policy", "default-src *");
        detail::hardenRead(reply, "a.txt");
        THEN("its caching and policy are replaced, not duplicated") {
            REQUIRE(headers(reply, "Cache-Control") == std::vector<std::string>{"no-store"});
            REQUIRE(headers(reply, "Content-Security-Policy") == std::vector<std::string>{"sandbox; default-src 'none'"});
        }
    }
    GIVEN("a refusal") {
        Fixture fixture;
        const auto denied = call(fixture.read, std::string(prefix) + "nothing.here/x");
        THEN("it is neutralized too, and tells nothing") {
            REQUIRE(denied.status == http::Response::forbidden);
            REQUIRE(header(denied, "Cache-Control") == "no-store");
            REQUIRE(header(denied, "Referrer-Policy") == "no-referrer");
        }
    }
}

namespace {

struct NoFiles {
    static inline std::list<std::string_view> names{};
};

/// One connection, accepted at once.
struct ReadNet : testing::InjectableNetworking {
    static inline std::atomic<int> accepts{0};
    static auto                    Accept(auto&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (accepts++ != 0)
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

bool waitFor(auto condition) {
    for (auto waited = 0ms; waited < 10s; waited += 1ms) {
        if (condition())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

}  // namespace

SCENARIO("The server never writes a read token to its logs", "[read-binding]") {
    testing::InjectableSocket::reset();
    ReadNet::accepts = 0;
    http::Server<ReadNet, MockFileSystem<NoFiles>> server("127.0.0.1", "0");
    Fixture                                                 fixture;
    const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
    const auto url    = fixture.url(1, "secret-name.txt");
    auto       route  = server.addRoute(std::string(prefix), fixture.read, true);

    webfront::log::setLogLevel(webfront::log::Debug);
    std::string logged;
    std::mutex  loggedMutex;
    const auto  sink = webfront::log::addSinks([&](std::string_view line) {
        std::scoped_lock lock{loggedMutex};
        logged += std::string(line) + "\n";
    });
    std::string raw = "GET " + url + " HTTP/1.1\r\nHost: localhost\r\nCookie: " + cookie + "\r\n\r\n";
    std::vector<std::byte> bytes;
    for (const char c : raw)
        bytes.push_back(static_cast<std::byte>(c));
    testing::InjectableSocket::receive(bytes);
    REQUIRE(waitFor([] { return testing::InjectableSocket::shutdownCount() > 0; }));
    webfront::log::removeSinks(sink);
    webfront::log::setLogLevel(webfront::log::Error);

    THEN("the response is the neutralized read, and no log line holds the token") {
        const auto response = testing::InjectableSocket::writtenText();
        REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(response.contains("Content-Security-Policy: sandbox; default-src 'none'\r\n"));
        const auto token = url.substr(prefix.size(), 16 + 1 + 64);
        std::scoped_lock lock{loggedMutex};
        REQUIRE(logged.contains("Received request GET on /webfront/services/files/..."));
        REQUIRE_FALSE(logged.contains(token));
        REQUIRE_FALSE(logged.contains(token.substr(0, 16)));
        REQUIRE_FALSE(logged.contains("secret-name"));
    }
    testing::InjectableSocket::reset();
}

namespace {

using ReadApp = BasicWF<ReadNet, fs::DefaultFS>;

}  // namespace

SCENARIO("An application registers read routes under the services path only", "[read-binding]") {
    testing::InjectableSocket::reset();
    ReadNet::accepts = 100;
    ReadApp app{WebFrontConfig{.port = "0"}};
    const auto handler = [](const http::RouteRequest&, const ReadGrant&) { return http::RouteReply{}; };
    THEN("a prefix outside the cookie's path, the path itself and the exchange route are refused") {
        REQUIRE_THROWS_AS(static_cast<void>(app.addReadRoute("/files/", handler)), std::invalid_argument);
        REQUIRE_THROWS_AS(static_cast<void>(app.addReadRoute("/webfront/services/", handler)), std::invalid_argument);
        REQUIRE_THROWS_AS(static_cast<void>(app.addReadRoute("/webfront/services/bind/", handler)), std::invalid_argument);
        REQUIRE_THROWS_AS(static_cast<void>(app.addReadRoute("/webfront/services/bind/more/", handler)), std::invalid_argument);
        REQUIRE_THROWS_AS(static_cast<void>(app.addReadRoute("/webfront/services/files/", nullptr)), std::invalid_argument);
    }
    THEN("a prefix under it is accepted once, then free again when the route is dropped") {
        {
            auto route = app.addReadRoute("/webfront/services/files/", handler);
            REQUIRE(route);
            REQUIRE_THROWS_AS(static_cast<void>(app.addReadRoute("/webfront/services/files/", handler)), std::invalid_argument);
        }
        REQUIRE_NOTHROW(static_cast<void>(app.addReadRoute("/webfront/services/files/", handler)));
    }
    THEN("a loopback declaration is refused for an application that listens on all interfaces") {
        REQUIRE_THROWS_AS(ReadApp(WebFrontConfig{.address = "0.0.0.0", .port = "0", .remoteFileTransport = RemoteFileTransport::loopbackAttested}),
                          std::invalid_argument);
    }
    testing::InjectableSocket::reset();
}
