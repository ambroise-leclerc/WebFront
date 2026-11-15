// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief The binding of HTTP reads to the browser client of a connection: token, cookie, transport, neutralized content (#469, ADR-020).
#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include "Bdd.hpp"
#include <webfront/ReadBinding.hpp>
#include <webfront/WebFront.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/tooling/Logger.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view readPrefix = "/webfront/services/files/";

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
    require(request.completed());
    const auto split = http::routing::splitTarget(path);  // the server decodes the target before dispatch
    require(split);
    return handler(http::RouteRequest{request, split->first, split->second});
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
        read = binding.readHandler("route-a", std::string(readPrefix), [this](const http::RouteRequest&, const ReadGrant& grant) {
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
        return binding.grant(key, "route-a", readPrefix, path, std::move(options));
    }
};

}  // namespace

const Scenario readNeeds{"A read needs the token and the cookie of the client of the connection that owns it", {"read-binding"}, [] {
    Fixture fixture;
    given("a local connection that exchanged its secret for a cookie", [&] {
        const auto bound = fixture.bind(1, true);
        const auto cookies = headers(bound, "Set-Cookie");
        require(bound.status == http::Response::ok);
        require(cookies.size() == 1);
        const auto cookie = cookiePair(cookies.front());
        const auto url    = fixture.url(1, "dir/a b.txt");

        then("the cookie is named after its client and restricted", [&] {
            require(cookie.starts_with("wf-client-"));
            require(cookies.front().contains("; Path=/webfront/services/"));
            require(cookies.front().contains("; HttpOnly"));
            require(cookies.front().contains("; SameSite=Strict"));
            require(!cookies.front().contains("Secure"));
        });
        then("the URL is under the readPrefix, followed by the token and the encoded path", [&] {
            require(url.starts_with(readPrefix));
            require(url.ends_with("/dir/a%20b.txt"));
        });
        when("the owner's client presents the URL with its cookie", [&] {
            const auto reply = call(fixture.read, url, cookie);
            then("the handler runs with the decoded rest of the path", [&] {
                require(reply.status == http::Response::ok);
                require(fixture.lastRest == "dir/a b.txt");
            });
        });
        when("the URL is presented without cookie", [&] {
            require(call(fixture.read, url).status == http::Response::forbidden);
            then("the handler did not run", [&] { require(fixture.lastRest.empty()); });
        });
        when("the URL is presented with the cookie of another client", [&] {
            const auto other = cookiePair(headers(fixture.bind(2, true), "Set-Cookie").front());
            require(other != cookie);
            then("it is refused", [&] { require(call(fixture.read, url, other).status == http::Response::forbidden); });
        });
        when("the URL is presented with the right cookie name and a wrong value", [&] {
            const auto forged = cookie.substr(0, cookie.size() - 1) + (cookie.back() == '0' ? "1" : "0");
            then("it is refused", [&] { require(call(fixture.read, url, forged).status == http::Response::forbidden); });
        });
        when("the token is altered or absent", [&] {
            auto altered = url;
            altered[readPrefix.size() + 20] = altered[readPrefix.size() + 20] == 'a' ? 'b' : 'a';
            then("every refusal looks alike", [&] {
                const auto unknown = call(fixture.read, altered, cookie);
                const auto missing = call(fixture.read, std::string(readPrefix), cookie);
                const auto garbage = call(fixture.read, std::string(readPrefix) + "nodot/x", cookie);
                require(unknown.status == http::Response::forbidden);
                require(missing.status == http::Response::forbidden);
                require(garbage.status == http::Response::forbidden);
                require(unknown.headers == missing.headers);
                require(unknown.headers == garbage.headers);
            });
        });
        when("the URL is requested repeatedly, as a media element does for its byte ranges", [&] {
            then("the token stays valid", [&] {
                for (int range = 0; range < 5; ++range)
                    require(call(fixture.read, url, cookie).status == http::Response::ok);
            });
        });
        when("the owning connection closes", [&] {
            fixture.binding.close(1);
            then("the URL no longer works, cookie or not", [&] { require(call(fixture.read, url, cookie).status == http::Response::forbidden); });
        });
        when("the grant is revoked", [&] {
            fixture.binding.revoke(url);
            then("the URL no longer works", [&] { require(call(fixture.read, url, cookie).status == http::Response::forbidden); });
        });
        when("the server restarts: another binding sees the old cookie and URL", [&] {
            Fixture restarted;
            then("neither is known", [&] { require(call(restarted.read, url, cookie).status == http::Response::forbidden); });
            andThen("the old cookie, indistinguishable from another application's, is left alone (it is a session cookie)", [&] {
                const auto again = restarted.bind(1, true, cookie);
                for (const auto& line : headers(again, "Set-Cookie"))
                    require(!line.contains("Max-Age=0"));
            });
        });
    });
}};

const Scenario connectionsOne{"The connections of one browser profile belong to one client", {"read-binding"}, [] {
    Fixture fixture;
    given("a first tab that exchanged and a second tab that presents the cookie it already holds", [&] {
        const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
        const auto second = fixture.bind(2, true, cookie);
        then("the second tab creates no cookie", [&] { require(headers(second, "Set-Cookie").empty()); });
        const auto url1 = fixture.url(1);
        const auto url2 = fixture.url(2);
        then("each tab's URL is read with the shared cookie, as the same profile does", [&] {
            require(call(fixture.read, url1, cookie).status == http::Response::ok);
            require(call(fixture.read, url2, cookie).status == http::Response::ok);
        });
        andWhen("the first tab closes", [&] {
            fixture.binding.close(1);
            then("the client lives on with the second", [&] {
                require(call(fixture.read, url1, cookie).status == http::Response::forbidden);
                require(call(fixture.read, url2, cookie).status == http::Response::ok);
            });
            andWhen("the second closes too", [&] {
                fixture.binding.close(2);
                then("the client is forgotten: a third tab with the cookie gets a new client and the old cookie is erased", [&] {
                    const auto third = fixture.bind(3, true, cookie);
                    const auto lines = headers(third, "Set-Cookie");
                    require(lines.size() == 2);
                    require(lines[0].contains("Max-Age=0"));
                    require(cookiePair(lines[1]) != cookie);
                });
            });
        });
    });
    given("two tabs of a blank profile whose exchanges run at the same time", [&] {
        // Both secrets are issued, and both requests are answered, before the browser stores either cookie.
        const auto secretA = fixture.binding.open(1, true);
        const auto secretB = fixture.binding.open(2, true);
        const auto answerA = call(fixture.exchange, std::string(ReadBinding::bindPath) + secretA);
        const auto answerB = call(fixture.exchange, std::string(ReadBinding::bindPath) + secretB);
        const auto cookieA = cookiePair(headers(answerA, "Set-Cookie").front());
        const auto cookieB = cookiePair(headers(answerB, "Set-Cookie").front());
        then("the cookies have different names, so neither overwrites the other", [&] {
            require(cookieA.substr(0, cookieA.find('=')) != cookieB.substr(0, cookieB.find('=')));
        });
        then("the browser sends both, and both tabs read their URLs", [&] {
            const auto both = cookieA + "; " + cookieB;
            require(call(fixture.read, fixture.url(1), both).status == http::Response::ok);
            require(call(fixture.read, fixture.url(2), both).status == http::Response::ok);
        });
        then("each cookie alone opens its own tab's URL only", [&] {
            require(call(fixture.read, fixture.url(1), cookieA).status == http::Response::ok);
            require(call(fixture.read, fixture.url(2), cookieA).status == http::Response::forbidden);
        });
    });
}};

const Scenario exchangeSecret{"The exchange secret is single use and only the right one works", {"read-binding"}, [] {
    Fixture fixture;
    const auto secret = fixture.binding.open(1, true);
    const auto path   = std::string(ReadBinding::bindPath) + secret;
    then("a wrong secret is refused and gives no cookie", [&] {
        const auto reply = call(fixture.exchange, std::string(ReadBinding::bindPath) + std::string(64, '0'));
        require(reply.status == http::Response::forbidden);
        require(headers(reply, "Set-Cookie").empty());
    });
    then("the secret works once", [&] {
        require(call(fixture.exchange, path).status == http::Response::ok);
        const auto replay = call(fixture.exchange, path);
        require(replay.status == http::Response::forbidden);
        require(headers(replay, "Set-Cookie").empty());
    });
    then("a secret does not survive its connection", [&] {
        fixture.binding.close(1);
        require(call(fixture.exchange, path).status == http::Response::forbidden);
    });
    then("a connection key opened twice starts over", [&] {
        const auto again = fixture.binding.open(1, true);
        require(call(fixture.exchange, path).status == http::Response::forbidden);
        require(call(fixture.exchange, std::string(ReadBinding::bindPath) + again).status == http::Response::ok);
    });
    then("the check of one exchange is not satisfied by the cookie of a concurrent one", [&] {
        // The browser kept the first cookie and rejected the second: the second client must not be told it is bound.
        const auto cookieA = cookiePair(headers(call(fixture.exchange, path), "Set-Cookie").front());
        const auto clientB = *header(fixture.bind(2, true), "X-WebFront-Client");
        require(call(fixture.exchange, std::string(ReadBinding::bindPath) + "check/" + clientB, cookieA).status == http::Response::forbidden);
    });
    then("the check answers only to a request whose cookies designate the client the exchange named", [&] {
        const auto base = std::string(ReadBinding::bindPath) + "check/";
        const auto reply  = call(fixture.exchange, path);
        const auto client = *header(reply, "X-WebFront-Client");
        const auto cookie = cookiePair(headers(reply, "Set-Cookie").front());
        require(call(fixture.exchange, base + client).status == http::Response::forbidden);
        require(call(fixture.exchange, base + client, cookie).status == http::Response::ok);
        require(call(fixture.exchange, base + "0000000000000000", cookie).status == http::Response::forbidden);
        require(call(fixture.exchange, base + "0000000000000000", fixture.binding.clientCookiePrefix() + "0000000000000000=" + std::string(64, 'a')).status == http::Response::forbidden);
    });
}};

const Scenario grantWorks{"A grant works only on the route registration that issued it", {"read-binding"}, [] {
    Fixture fixture;
    const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
    const auto url    = fixture.url(1, "a.txt", {.context = std::string("secret-of-route-a")});
    bool       otherRan = false;
    // A later route on the same readPrefix (route A was dropped and B registered), or a sibling readPrefix.
    const auto other = fixture.binding.readHandler("route-b", std::string(readPrefix), [&](const http::RouteRequest&, const ReadGrant&) {
        otherRan = true;
        return http::RouteReply{};
    });
    then("the other route refuses the token without running its handler", [&] {
        require(call(other, url, cookie).status == http::Response::forbidden);
        require(!otherRan);
    });
    then("the issuing route still accepts it", [&] { require(call(fixture.read, url, cookie).status == http::Response::ok); });
}};

const Scenario transferableMode{"The transferable mode makes the token alone a short-lived bearer capability", {"read-binding"}, [] {
    Fixture fixture;
    const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
    then("it needs an explicit short lifetime", [&] {
        requireThrows<ReadRefused>([&] { fixture.url(1, "a", {.transferable = true}); });
        requireThrows<ReadRefused>([&] { fixture.url(1, "a", {.lifetime = 16min, .transferable = true}); });
        requireThrows<ReadRefused>([&] { fixture.url(1, "a", {.lifetime = -1s}); });
    });
    given("a transferable URL", [&] {
        const auto url = fixture.url(1, "a", {.lifetime = 60s, .transferable = true});
        then("a client without any cookie reads it", [&] { require(call(fixture.read, url).status == http::Response::ok); });
        then("it stops at its expiry", [&] {
            fixture.time += 59s;
            require(call(fixture.read, url).status == http::Response::ok);
            fixture.time += 2s;
            require(call(fixture.read, url).status == http::Response::forbidden);
        });
        then("it stops with the connection", [&] {
            fixture.binding.close(1);
            require(call(fixture.read, url).status == http::Response::forbidden);
        });
    });
    given("a bound URL with a lifetime", [&] {
        const auto url = fixture.url(1, "a", {.lifetime = 60s});
        then("the cookie is still required, and the lifetime still applies", [&] {
            require(call(fixture.read, url).status == http::Response::forbidden);
            require(call(fixture.read, url, cookie).status == http::Response::ok);
            fixture.time += 61s;
            require(call(fixture.read, url, cookie).status == http::Response::forbidden);
        });
    });
    given("a lifetime the clock cannot represent", [&] {
        then("it is refused instead of overflowing the expiry, and a lifetime that fits is accepted", [&] {
            requireThrows<ReadRefused>([&] { fixture.url(1, "a", {.lifetime = std::chrono::seconds::max()}); });
            const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(Fixture::Clock::time_point::max() - fixture.time);
            requireThrows<ReadRefused>([&] { fixture.url(1, "a", {.lifetime = remaining + 1s}); });
            const auto url = fixture.url(1, "a", {.lifetime = remaining - 1s});
            require(call(fixture.read, url, cookie).status == http::Response::ok);
        });
    });
    then("the context of the grant reaches the handler", [&] {
        const auto url = fixture.url(1, "a", {.context = std::string("handle-7")});
        require(call(fixture.read, url, cookie).status == http::Response::ok);
        require(std::any_cast<std::string>(fixture.lastContext) == "handle-7");
    });
}};

const Scenario connectionNot{"A connection that is not the view of an embedded frontend reads only under a declared transport", {"read-binding"}, [] {
    given("the default declaration", [&] {
        Fixture fixture;
        const auto cookie = cookiePair(headers(fixture.bind(1, false), "Set-Cookie").front());
        then("no URL is given to the system browser", [&] { requireThrows<ReadRefused>([&] { fixture.url(1); }); });
        then("a connection that is closed has none either", [&] { requireThrows<ReadRefused>([&] { fixture.url(9); }); });
        then("the embedded view reads", [&] {
            const auto viewCookie = cookiePair(headers(fixture.bind(2, true), "Set-Cookie").front());
            require(call(fixture.read, fixture.url(2), viewCookie).status == http::Response::ok);
        });
    });
    for (const auto transport : {RemoteFileTransport::loopbackAttested, RemoteFileTransport::encryptedByDeployment, RemoteFileTransport::httpsByDeployment, RemoteFileTransport::plaintextAccepted}) {
        given("the declared transport ", static_cast<int>(transport), [&] {
            Fixture fixture{transport};
            const auto bound  = fixture.bind(1, false);
            const auto cookie = cookiePair(headers(bound, "Set-Cookie").front());
            const auto url    = fixture.url(1);
            then("the system browser reads with its cookie, whatever the forwarding headers claim", [&] {
                require(call(fixture.read, url, cookie, "X-Forwarded-Proto: https\r\nForwarded: for=127.0.0.1;proto=https\r\n").status == http::Response::ok);
                require(call(fixture.read, url).status == http::Response::forbidden);
            });
            then("the cookie is Secure only when the browser is declared to use https://, not for an encrypted tunnel under http://", [&] {
                require(headers(bound, "Set-Cookie").front().contains("; Secure") == (transport == RemoteFileTransport::httpsByDeployment));
            });
        });
    }
    given("a request through a local proxy that forges the forwarding headers, with the default declaration", [&] {
        Fixture fixture;
        fixture.bind(1, false);
        then("nothing makes the connection local: the headers and the address are never consulted", [&] {
            requireThrows<ReadRefused>([&] { fixture.url(1); });
        });
    });
}};

const Scenario declarationLoopback{"A declaration of a loopback server is checked against the address it listens on", {"read-binding"}, [] {
    for (const auto* address : {"127.0.0.1", "127.1.2.3", "::1", "[::1]", "0:0:0:0:0:0:0:1", "0000:0000:0000:0000:0000:0000:0000:0001", "0::1", "::0001", "[0:0::1]"})
        then("'", address, "' is a loopback address", [&] { require(detail::isLoopbackName(address)); });
    for (const auto* address : {"0.0.0.0", "::", "", "192.168.1.5", "10.0.0.1", "127.0.0", "127.0.0.1.5", "127.a.0.1", "127..0.1", "example.com", "localhost", "LocalHost", "127.0.0.1.", "127.256.0.1", "127.0.256.1", "127.0.0.1000", "127.0.0.010", "127.00.0.1", "127.0.0.", ":::1", "0:0:0:0:0:0:0:2", "1:0:0:0:0:0:0:1", "0:0:0:0:0:0:0:0:1", "::1::", "::12345", "::g"})
        then("'", address, "' is not", [&] { require(!detail::isLoopbackName(address)); });
    then("loopbackAttested is refused for a server that listens elsewhere, and accepted on the loopback", [&] {
        requireThrows<std::invalid_argument>([&] { detail::checkedAddress(WebFrontConfig{.address = "0.0.0.0", .remoteFileTransport = RemoteFileTransport::loopbackAttested}); });
        requireNothrow([&] { detail::checkedAddress(WebFrontConfig{.address = "127.0.0.1", .remoteFileTransport = RemoteFileTransport::loopbackAttested}); });
        requireNothrow([&] { detail::checkedAddress(WebFrontConfig{.address = "0.0.0.0", .remoteFileTransport = RemoteFileTransport::plaintextAccepted}); });
    });
}};

const Scenario readResponses{"Read responses neutralize active content", {"read-binding"}, [] {
    const auto present = [](std::string contentType, std::string rest = "dir/file.bin") {
        http::RouteReply reply;
        if (!contentType.empty())
            reply.headers.emplace_back("Content-Type", std::move(contentType));
        reply.headers.emplace_back("Content-Disposition", "inline");  // a handler cannot choose
        detail::hardenRead(reply, rest);
        return reply;
    };
    given("any response", [&] {
        const auto reply = present("image/png");
        then("it carries the protective headers", [&] {
            require(header(reply, "Cache-Control") == "no-store");
            require(header(reply, "Referrer-Policy") == "no-referrer");
            require(header(reply, "Content-Security-Policy") == "sandbox; default-src 'none'");
            require(header(reply, "X-Content-Type-Options") == "nosniff");
            require(header(reply, "Cross-Origin-Resource-Policy") == "same-origin");
        });
    });
    given("passive types", [&] {
        for (const auto* type : {"image/png", "image/jpeg", "image/gif", "image/webp", "audio/mpeg", "video/mp4", "text/plain", "text/plain; charset=utf-8", "VIDEO/WEBM"}) {
            then(type, " is shown inline with its type", [&] {
                const auto reply = present(type);
                require(header(reply, "Content-Type") == type);
                require(!header(reply, "Content-Disposition"));
            });
        }
    });
    given("active types", [&] {
        for (const auto* type : {"text/html", "text/html; charset=utf-8", "application/xhtml+xml", "text/xml", "application/xml", "application/atom+xml",
                                 "text/javascript", "application/javascript", "application/ecmascript", "text/javascript1.5", "application/x-ecmascript", "text/jscript", "text/livescript", "text/css", "application/wasm", "TEXT/HTML"}) {
            then(type, " is a download announced as octet-stream", [&] {
                const auto reply = present(type);
                require(header(reply, "Content-Type") == "application/octet-stream");
                require(header(reply, "Content-Disposition")->starts_with("attachment; filename=\"file.bin\""));
            });
        }
    });
    given("an SVG", [&] {
        const auto reply = present("image/svg+xml", "logo.svg");
        then("it keeps its type, for <img>, and a navigation downloads it", [&] {
            require(header(reply, "Content-Type") == "image/svg+xml");
            require(header(reply, "Content-Disposition")->starts_with("attachment; filename=\"logo.svg\""));
        });
    });
    given("a type outside the closed list, or none", [&] {
        then("it is a download", [&] {
            require(header(present("application/pdf"), "Content-Disposition")->starts_with("attachment"));
            require(header(present("application/pdf"), "Content-Type") == "application/pdf");
            require(header(present("text/css"), "Content-Disposition")->starts_with("attachment"));
            require(header(present(""), "Content-Type") == "application/octet-stream");
            require(header(present(""), "Content-Disposition")->starts_with("attachment"));
        });
    });
    given("a hostile file name", [&] {
        const auto reply = present("text/html", "a\"b\r\nSet-Cookie: x;\\c/é.html");
        then("the header stays one well-formed line", [&] {
            const auto disposition = *header(reply, "Content-Disposition");
            require(disposition.find('\r') == std::string::npos);
            require(disposition.find('\n') == std::string::npos);
            require(disposition.starts_with("attachment; filename=\""));
            require(disposition.contains("filename*=UTF-8''"));
            require(disposition.contains("%C3%A9.html"));
        });
    });
    given("a response a handler built with headers of its own", [&] {
        http::RouteReply reply;
        reply.headers.emplace_back("Content-Type", "text/plain");
        reply.headers.emplace_back("cache-control", "public, max-age=3600");
        reply.headers.emplace_back("Content-Security-Policy", "default-src *");
        detail::hardenRead(reply, "a.txt");
        then("its caching and policy are replaced, not duplicated", [&] {
            require(headers(reply, "Cache-Control") == std::vector<std::string>{"no-store"});
            require(headers(reply, "Content-Security-Policy") == std::vector<std::string>{"sandbox; default-src 'none'"});
        });
    });
    given("a refusal", [&] {
        Fixture fixture;
        const auto denied = call(fixture.read, std::string(readPrefix) + "nothing.here/x");
        then("it is neutralized too, and tells nothing", [&] {
            require(denied.status == http::Response::forbidden);
            require(header(denied, "Cache-Control") == "no-store");
            require(header(denied, "Referrer-Policy") == "no-referrer");
        });
    });
}};

namespace {

/// Every log line written at Debug level until stop() or scope exit: a failing REQUIRE must not leave a sink that points
/// into the test.
class LogCapture {
public:
    LogCapture() {
        webfront::log::setLogLevel(webfront::log::Debug);
        sink = webfront::log::addSinks(std::function<void(std::string_view)>([this](std::string_view line) {
            std::scoped_lock lock{mutex};
            logged += std::string(line) + "\n";
        }));
    }
    ~LogCapture() { stop(); }
    void stop() {
        if (std::exchange(active, false)) {
            webfront::log::removeSinks(sink);
            webfront::log::setLogLevel(webfront::log::Error);
        }
    }
    std::string text() {
        std::scoped_lock lock{mutex};
        return logged;
    }

private:
    std::mutex  mutex;
    std::string logged;
    decltype(webfront::log::addSinks(std::function<void(std::string_view)>{})) sink{};
    bool        active = true;
};

struct NoFiles {
    static inline std::list<std::string_view> names{};
};

/// A file provider whose failure quotes the path it was asked for.
struct ThrowingFileSystem {
    ThrowingFileSystem(std::filesystem::path) {}
    std::optional<webfront::fs::File> open(std::filesystem::path file) { throw std::runtime_error("cannot open " + file.string()); }
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

/// The injected socket starts clean before the server exists and is cleaned once it is gone.
struct CleanSocket {
    CleanSocket() {
        testing::InjectableSocket::reset();
        ReadNet::accepts = 0;
    }
    ~CleanSocket() { testing::InjectableSocket::reset(); }
};

/// A server whose single connection receives one request: answer() returns what was logged meanwhile, and response() what
/// the server wrote.
template <typename Files = MockFileSystem<NoFiles>>
struct OneRequest : CleanSocket {
    http::Server<ReadNet, Files> server{"127.0.0.1", "0"};

    /// Sends "<requestLine> HTTP/1.1" with `headers` and waits for the server to close the connection.
    std::string answer(const std::string& requestLine, const std::string& headers = {}) {
        LogCapture             log;
        const std::string      raw = requestLine + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
        std::vector<std::byte> bytes;
        for (const char c : raw)
            bytes.push_back(static_cast<std::byte>(c));
        testing::InjectableSocket::receive(bytes);
        requireEventually(
            [] {
                return testing::InjectableSocket::shutdownCount() > 0;
            },
            10s);
        log.stop();
        return log.text();
    }
    static std::string response() { return testing::InjectableSocket::writtenText(); }
};

}  // namespace

const Scenario serverNever{"The server never writes a read token to its logs", {"read-binding"}, [] {
    OneRequest<> request;
    Fixture      fixture;
    const auto   cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
    const auto   url    = fixture.url(1, "secret-name.txt");
    auto         route  = request.server.addRoute(std::string(readPrefix), fixture.read, true);
    const auto   logged = request.answer("GET " + url, "Cookie: " + cookie + "\r\n");

    then("the response is the neutralized read, and no log line holds the token", [&] {
        const auto response = request.response();
        require(response.starts_with("HTTP/1.1 200 OK\r\n"));
        require(response.contains("Content-Security-Policy: sandbox; default-src 'none'\r\n"));
        const auto token = url.substr(readPrefix.size(), 16 + 1 + 64);
        require(logged.contains("Received request GET on /webfront/services/files/..."));
        require(!logged.contains(token));
        require(!logged.contains(token.substr(0, 16)));
        require(!logged.contains("secret-name"));
    });
}};

const Scenario noLog{"No log line holds the token of a services target, whatever answers it", {"read-binding"}, [] {
    const std::string target = std::string(readPrefix) + "0123456789abcdef.TOKENSECRET/a.txt";
    then("a target unfit for route dispatch, plain or spelled with an encoded letter and doubled separators, falls to the "
         "file providers without being logged", [&] {
        // "report..txt" makes the target unfit for route dispatch.
        for (const auto* unfit : {"/webfront/services/files/0123456789abcdef.TOKENSECRET/report..txt",
                                  "/%77ebfront//services/./files/0123456789abcdef.TOKENSECRET/report..txt"}) {
            OneRequest<> request;
            const auto   logged = request.answer(std::string("GET ") + unfit);
            require(logged.contains("Received request GET"));
            require(!logged.contains("TOKENSECRET"));
            require(!logged.contains("report"));
        }
    });
    then("the error of a failing secret route is not logged", [&] {
        OneRequest<> request;
        auto         route = request.server.addRoute(std::string(readPrefix), [](const http::RouteRequest& failed) -> http::RouteReply {
            throw std::runtime_error("cannot open " + std::string(failed.path));
        }, true);
        const auto logged = request.answer("GET " + target);
        require(logged.contains("failed"));
        require(!logged.contains("TOKENSECRET"));
    });
    then("a file provider that fails on the target, with no route registered, does not log it", [&] {
        OneRequest<ThrowingFileSystem> request;
        const auto                     logged = request.answer("GET " + target);
        require(logged.contains("failed"));
        require(!logged.contains("TOKENSECRET"));
    });
    then("a secret route lives under the services space, and the query of a request is never logged", [&] {
        OneRequest<> request;
        requireThrows<std::invalid_argument>([&] { (void)request.server.addRoute("/files/", [](const http::RouteRequest&) { return http::RouteReply{}; }, true); });
        const auto logged = request.answer("GET /viewer?file=%2Fwebfront%2Fservices%2Ffiles%2FQUERYSECRET%2Fdoc");
        require(logged.contains("Received request GET on /viewer"));
        require(!logged.contains("QUERYSECRET"));
    });
}};

const Scenario answersServer{"The answers the server writes for a read route are hardened too", {"read-binding"}, [] {
    for (const auto* method : {"GET", "POST"}) {
        OneRequest<> request;
        auto route = request.server.addRoute(std::string(readPrefix), [](const http::RouteRequest&) -> http::RouteReply { throw std::runtime_error("boom"); }, true,
                                             [](http::RouteReply& reply) { detail::hardenRead(reply, {}); });
        request.answer(std::string(method) + " " + std::string(readPrefix) + "0123456789abcdef.SECRET/a.txt");
        const auto response = request.response();
        then(std::string(method) + " is refused with the hardening headers", [&] {
            require(response.starts_with(std::string_view(method) == "GET" ? "HTTP/1.1 500" : "HTTP/1.1 405"));
            require(response.contains("Cache-Control: no-store\r\n"));
            require(response.contains("Referrer-Policy: no-referrer\r\n"));
            require(response.contains("Content-Security-Policy: sandbox; default-src 'none'\r\n"));
            require(response.contains("X-Content-Type-Options: nosniff\r\n"));
        });
    }
}};

const Scenario frameNever{"A frame never logs the bytes of its payload", {"read-binding"}, [] {
    LogCapture             log;
    std::vector<std::byte> bytes(64, static_cast<std::byte>('q'));
    websocket::Frame<ReadNet> frame{std::string_view{}};
    frame.addBuffer(bytes);
    log.stop();
    require(!log.text().contains("qqqq"));
    require(!log.text().contains("71 71"));
}};

namespace {

using ReadApp = BasicWF<ReadNet, fs::DefaultFS>;

}  // namespace

const Scenario applicationRegisters{"An application registers read routes under the services path only", {"read-binding"}, [] {
    testing::InjectableSocket::reset();
    ReadNet::accepts = 100;
    ReadApp app{WebFrontConfig{.port = "0"}};
    const auto handler = [](const http::RouteRequest&, const ReadGrant&) { return http::RouteReply{}; };
    then("a readPrefix outside the cookie's path, the path itself and the exchange route are refused", [&] {
        requireThrows<std::invalid_argument>([&] { static_cast<void>(app.addReadRoute("/files/", handler)); });
        requireThrows<std::invalid_argument>([&] { static_cast<void>(app.addReadRoute("/webfront/services/", handler)); });
        requireThrows<std::invalid_argument>([&] { static_cast<void>(app.addReadRoute("/webfront/services/bind/", handler)); });
        requireThrows<std::invalid_argument>([&] { static_cast<void>(app.addReadRoute("/webfront/services/bind/more/", handler)); });
        requireThrows<std::invalid_argument>([&] { static_cast<void>(app.addReadRoute("/webfront/services/files/", nullptr)); });
    });
    then("a readPrefix under it is accepted once, then free again when the route is dropped", [&] {
        {
            auto route = app.addReadRoute("/webfront/services/files/", handler);
            require(route);
            requireThrows<std::invalid_argument>([&] { static_cast<void>(app.addReadRoute("/webfront/services/files/", handler)); });
        }
        requireNothrow([&] { static_cast<void>(app.addReadRoute("/webfront/services/files/", handler)); });
    });
    then("a route of another application grants nothing to this one's browser", [&] {
        ReadApp other{WebFrontConfig{.port = "0"}};
        auto    foreign = other.addReadRoute("/webfront/services/files/", handler);
        ReadApp::UI ui{app, 0, 1};
        requireThrows([&] { (void)ui.grantRead(foreign, "a.txt"); }, contains("another application"));
    });
    then("a loopback declaration is refused for an application that listens on all interfaces", [&] {
        requireThrows<std::invalid_argument>([&] { ReadApp(WebFrontConfig{.address = "0.0.0.0", .port = "0", .remoteFileTransport = RemoteFileTransport::loopbackAttested}); });
    });
    testing::InjectableSocket::reset();
}};

const Scenario eachApplication{"Each application names its own cookies, and a namespace named after the port survives a restart", {"read-binding"}, [] {
    given("a client bound to each of two applications on one host, whose cookies the browser shares", [&] {
        Fixture    first, second;
        const auto cookieA = cookiePair(headers(first.bind(1, true), "Set-Cookie").front());
        const auto both    = cookieA + "; " + cookiePair(headers(second.bind(1, true), "Set-Cookie").front());
        require(cookieA.starts_with("wf-client-"));
        require(first.binding.clientCookiePrefix() != second.binding.clientCookiePrefix());

        then("binding the second one erases nothing of the first, and the reads still pass", [&] {
            const auto again = second.bind(2, true, both);
            for (const auto& setCookie : headers(again, "Set-Cookie"))
                require(!setCookie.contains("Max-Age=0"));
            const auto url = first.url(1);
            require(call(first.read, url, both).status == http::Response::ok);
        });
    });
    given("an application restarted on the same port", [&] {
        Fixture before, after;
        before.binding.useNamespace("9002");
        after.binding.useNamespace("9002");
        const auto cookie = cookiePair(headers(before.bind(1, true), "Set-Cookie").front());
        then("it recognizes the cookie of its previous run and erases it", [&] {
            require(cookie.starts_with("wf-client-9002-"));
            bool erased = false;
            for (const auto& line : headers(after.bind(1, true, cookie), "Set-Cookie"))
                erased = erased || (line.starts_with(cookie.substr(0, cookie.find('=')) + "=;") && line.contains("Max-Age=0"));
            require(erased);
        });
    });
}};

const Scenario tokenOpens{"A token opens only the path it was issued for, and only a path the server would route", {"read-binding"}, [] {
    Fixture fixture;
    const auto cookie = cookiePair(headers(fixture.bind(1, true), "Set-Cookie").front());
    const auto url    = fixture.url(1, "public.txt");
    require(call(fixture.read, url, cookie).status == http::Response::ok);
    const auto other = url.substr(0, url.rfind('/')) + "/private.txt";
    require(call(fixture.read, other, cookie).status == http::Response::forbidden);
    require(fixture.lastRest == "public.txt");
    const auto transferable = fixture.url(1, "public.txt", {.lifetime = 60s, .transferable = true});
    require(call(fixture.read, transferable).status == http::Response::ok);
    require(call(fixture.read, transferable.substr(0, transferable.rfind('/')) + "/private.txt").status == http::Response::forbidden);
    for (const std::string_view path : {"a//b", "report..txt", "../up.txt", "a/../b", "tab\there.txt", "line\nbreak"})
        requireThrows<ReadRefused>([&] { fixture.url(1, path); });
    requireNothrow([&] { fixture.url(1, "dir/report.v2.txt"); });
}};

const Scenario grantContext{"A grant's context is released when the grant expires or the binding shuts down", {"read-binding"}, [] {
    Fixture fixture;
    fixture.bind(1, true);
    auto               held  = std::make_shared<int>(7);
    std::weak_ptr<int> watch = held;
    when("the grant expires", [&] {
        (void)fixture.url(1, "a", {.lifetime = 60s, .context = held});
        held.reset();
        require(!watch.expired());
        fixture.time += 61s;
        (void)fixture.url(1, "b");  // issuing another grant sweeps the expired ones
        then("its context is released without the grant being requested again", [&] { require(watch.expired()); });
    });
    when("the binding shuts down", [&] {
        const auto url = fixture.url(1, "a", {.context = held});
        held.reset();
        require(!watch.expired());
        fixture.binding.shutdown();
        then("it holds nothing and grants nothing", [&] {
            require(watch.expired());
            requireThrows<ReadRefused>([&] { fixture.url(1, "b"); });
            require(call(fixture.read, url).status == http::Response::forbidden);
        });
    });
}};

const Scenario droppingGrant{"Dropping a grant never runs its context's destructor under the lock", {"read-binding"}, [] {
    Fixture fixture;
    fixture.bind(1, true);
    // A context whose deleter calls back into the binding would deadlock if it ran while the binding is locked.
    const auto reentrant = [&fixture] {
        return std::shared_ptr<int>(new int(1), [&fixture](int* value) {
            delete value;
            (void)fixture.binding.accepts("route-a", "none", "x", {});
        });
    };
    when("the grant expires", [&] {
        (void)fixture.url(1, "a", {.lifetime = 1s, .context = reentrant()});
        fixture.time += 2s;
        then("the next grant sweeps it", [&] { requireNothrow([&] { fixture.url(1, "b"); }); });
    });
    when("the grant is revoked", [&] {
        fixture.binding.revoke(fixture.url(1, "a", {.context = reentrant()}));
    });
    when("its connection closes", [&] {
        (void)fixture.url(1, "a", {.context = reentrant()});
        fixture.binding.close(1);
    });
    when("the application shuts down", [&] {
        (void)fixture.url(1, "a", {.context = reentrant()});
        fixture.binding.shutdown();
    });
}};
