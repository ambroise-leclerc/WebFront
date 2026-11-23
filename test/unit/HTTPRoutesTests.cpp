/// Dynamic routes of the HTTP server (#454): registration, priority, streamed replies, failures and shutdown.
#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include "TemporaryDirectory.hpp"
#include <webfront/http/BlockingWorker.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/testing/Bdd.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <list>
#include <string>
#include <string_view>
#include <thread>

using namespace webfront::bdd;
#ifndef _WIN32
    #include <sys/resource.h>
#endif

using namespace webfront;
using namespace webfront::http;
using namespace std::chrono_literals;
using webfront::testing::InjectableSocket;

namespace {

/// One connection, accepted at once, whose request the test delivers once its routes are registered.
struct RouteNet : webfront::testing::InjectableNetworking {
    static inline std::atomic<int> accepts{0};
    static auto                    Accept(auto&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (accepts++ != 0)
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }
};

struct ProviderFiles {
    static inline std::list<std::string_view> names{"webfront/services/files/provided.txt"};
};
using Files  = MockFileSystem<ProviderFiles>;
using Router = Server<RouteNet, Files>;

void send(std::string_view request) {
    std::vector<std::byte> bytes;
    for (const char c : request)
        bytes.push_back(static_cast<std::byte>(c));
    InjectableSocket::receive(bytes);
}

std::string get(std::string_view target, std::string_view method = "GET") {
    return std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

/// A POST with the given header lines (each ending with CRLF) and the first bytes of its body.
std::string post(std::string_view target, std::string_view headers, std::string_view body = {}) {
    return "POST " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(headers) + "\r\n" + std::string(body);
}

/// Waits until the server asks for the read after `count` others: the bytes sent so far have been taken.
void awaitRead(int count) {
    requireEventually(
        [count] {
            return InjectableSocket::readCallCount() > count;
        },
        10s);
}

struct Fixture {
    Fixture() {
        InjectableSocket::reset();
        RouteNet::accepts = 0;
    }
    ~Fixture() {
        InjectableSocket::reset();
    }

    /// Sends the request and waits for the end of the connection; returns what the server wrote.
    std::string answer(std::string_view request) {
        send(request);
        requireEventually(
            [] {
                return InjectableSocket::shutdownCount() > 0;
            },
            10s);
        return InjectableSocket::writtenText();
    }
};

RouteReply text(std::string body) {
    RouteReply reply;
    reply.headers.emplace_back("Content-Type", "text/plain");
    reply.headers.emplace_back("Cache-Control", "no-store");
    reply.contentLength = body.size();
    reply.body          = [body = std::move(body), position = std::size_t{0}](std::span<char> out) mutable {
        const auto count = std::min(out.size(), body.size() - position);
        std::copy_n(body.data() + position, count, out.data());
        position += count;
        return count;
    };
    return reply;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

const Scenario routePrefixes{"Route prefixes are validated when registered", [] {
                                 RouteTable table;
                                 const auto handler = [](const RouteRequest&) {
                                     return RouteReply{};
                                 };
                                 given("prefixes that cannot be routes", [&] {
                                     for (const auto* prefix :
                                          {"", "/", "/files", "files/", "/a//b/", "/a/../b/", "/a/./b/", "/a b/", "/a?b/", "/a#/", "/a%2e/"}) {
                                         then("'", prefix, "' is refused", [&] {
                                             requireThrows<std::invalid_argument>([&] {
                                                 (void)table.add(prefix, handler);
                                             });
                                         });
                                     }
                                     then("a route without handler is refused", [&] {
                                         requireThrows<std::invalid_argument>([&] {
                                             (void)table.add("/webfront/services/files/", nullptr);
                                         });
                                     });
                                 });
                                 given("a registered prefix", [&] {
                                     auto registration = table.add("/webfront/services/files/", handler);
                                     then("the same prefix, a longer one and a shorter one collide", [&] {
                                         requireThrows<std::invalid_argument>([&] {
                                             (void)table.add("/webfront/services/files/", handler);
                                         });
                                         requireThrows<std::invalid_argument>([&] {
                                             (void)table.add("/webfront/services/files/deep/", handler);
                                         });
                                         requireThrows<std::invalid_argument>([&] {
                                             (void)table.add("/webfront/services/", handler);
                                         });
                                     });
                                     then("a sibling prefix, even sharing the name's start, is accepted", [&] {
                                         requireNothrow([&] {
                                             (void)table.add("/webfront/services/files2/", handler);
                                         });
                                         requireNothrow([&] {
                                             (void)table.add("/webfront/services/dialogs/", handler);
                                         });
                                     });
                                     then("lookup matches paths under the prefix only", [&] {
                                         require(table.find("/webfront/services/files/token"));
                                         require(!table.find("/webfront/services/files"));
                                         require(!table.find("/webfront/services/filesX/token"));
                                         require(!table.find("/WebFront.js"));
                                     });
                                     then("unregistering frees the prefix and the lookup", [&] {
                                         registration.reset();
                                         require(!table.find("/webfront/services/files/token"));
                                         requireNothrow([&] {
                                             (void)table.add("/webfront/services/files/", handler);
                                         });
                                     });
                                     then("a moved registration keeps the route until it is destroyed", [&] {
                                         auto moved = std::move(registration);
                                         require(table.find("/webfront/services/files/x"));
                                         moved = {};
                                         require(!table.find("/webfront/services/files/x"));
                                     });
                                 });
                                 given("a registration outliving its table", [&] {
                                     RouteTable::Registration registration;
                                     {
                                         RouteTable shortLived;
                                         registration = shortLived.add("/webfront/services/files/", handler);
                                     }
                                     then("destroying it is harmless", [&] {
                                         requireNothrow([&] {
                                             registration.reset();
                                         });
                                     });
                                 });
                             }};

const Scenario routeStreams{"A route streams its reply in bounded chunks", [] {
                                Fixture fixture;
                                Router  server("127.0.0.1", "0");
                                given("a body several chunks long", [&] {
                                    std::string body;
                                    for (std::size_t index = 0; index < 3 * DefaultBuffersPolicy::emissionBufferSize + 5; ++index)
                                        body.push_back(static_cast<char>('a' + index % 26));
                                    std::size_t largestChunk = 0;
                                    std::string target, query;
                                    auto        route = server.addRoute("/webfront/services/files/", [&](const RouteRequest& request) {
                                        target     = std::string(request.path);
                                        query      = std::string(request.query);
                                        auto reply = text(body);
                                        reply.body = [inner = std::move(reply.body), &largestChunk](std::span<char> out) {
                                            largestChunk = std::max(largestChunk, out.size());
                                            return inner(out);
                                        };
                                        return reply;
                                    });
                                    when("the browser requests it with a percent-encoded name and a query", [&] {
                                        const auto response = fixture.answer(get("/webfront/services/files/a%20b.txt?token=1"));
                                        then("the headers describe the reply and the whole body follows", [&] {
                                            require(response.starts_with("HTTP/1.1 200 OK\r\n"));
                                            require(contains(response, "Content-Type: text/plain\r\n"));
                                            require(contains(response, "Cache-Control: no-store\r\n"));
                                            require(contains(response, "Content-Length: " + std::to_string(body.size()) + "\r\n"));
                                            require(contains(response, "Connection: close\r\n"));
                                            require(response.ends_with("\r\n\r\n" + body));
                                        });
                                        then("the handler saw the decoded path and the raw query", [&] {
                                            require(target == "/webfront/services/files/a b.txt");
                                            require(query == "token=1");
                                        });
                                        then("the body was never asked for more than the emission buffer at once", [&] {
                                            require(largestChunk == DefaultBuffersPolicy::emissionBufferSize);
                                        });
                                    });
                                });
                            }};

const Scenario routeAnswers{"A route answers HEAD without reading its body", [] {
                                Fixture          fixture;
                                Router           server("127.0.0.1", "0");
                                std::atomic<int> reads{0};
                                auto             route    = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                    auto reply = text("0123456789");
                                    reply.body = [&reads](std::span<char>) {
                                        ++reads;
                                        return std::size_t{0};
                                    };
                                    return reply;
                                });
                                const auto       response = fixture.answer(get("/webfront/services/files/x", "HEAD"));
                                then("the headers announce the length and no body follows", [&] {
                                    require(response.starts_with("HTTP/1.1 200 OK\r\n"));
                                    require(contains(response, "Content-Length: 10\r\n"));
                                    require(response.ends_with("\r\n\r\n"));
                                    require(reads == 0);
                                });
                            }};

const Scenario routePrecedes{"A route precedes the file providers and refuses other methods", [] {
                                 Fixture          fixture;
                                 Router           server("127.0.0.1", "0");
                                 std::atomic<int> calls{0};
                                 auto             route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                     ++calls;
                                     return text("from the route");
                                 });
                                 given("a provider that also has a file under the prefix", [&] {
                                     const auto response = fixture.answer(get("/webfront/services/files/provided.txt"));
                                     then("the route answers", [&] {
                                         require(response.ends_with("from the route"));
                                     });
                                 });
                                 given("a POST", [&] {
                                     const auto response = fixture.answer(get("/webfront/services/files/x", "POST"));
                                     then("it is refused with the methods the route serves, without calling the handler", [&] {
                                         require(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
                                         require(contains(response, "Allow: GET, HEAD\r\n"));
                                         require(calls == 0);
                                     });
                                 });
                             }};

const Scenario routePosts{
    "A route that accepts POST reads a body bounded by Content-Length (#619)",
    [] {
        Fixture          fixture;
        Router           server("127.0.0.1", "0");
        std::atomic<int> calls{0};
        std::string      seen;
        auto             route = server.addRoute(
            "/webfront/services/rpc/",
            [&](const RouteRequest& request) {
                ++calls;
                seen = std::string(request.body);
                return text("echo:" + seen);
            },
            RouteOptions{.postBodyLimit = 64});
        given("a body received with the headers", [&] {
            const auto response = fixture.answer(post("/webfront/services/rpc/x", "Content-Length: 5\r\nContent-Type: application/json\r\n", "hello"));
            then("the handler gets the whole body and answers", [&] {
                require(response.starts_with("HTTP/1.1 200 OK\r\n"));
                require(response.ends_with("echo:hello"));
                require(seen == "hello");
            });
        });
        given("a client that waits for 100 Continue, then sends the body in two parts", [&] {
            send(post("/webfront/services/rpc/x", "Content-Length: 11\r\nExpect: 100-continue\r\n"));
            awaitRead(1);
            then("the server invites the body before the handler runs", [&] {
                require(InjectableSocket::writtenText() == "HTTP/1.1 100 Continue\r\n\r\n");
                require(calls == 0);
            });
            send("hello");
            awaitRead(2);
            send(" world");
            requireEventually(
                [] {
                    return InjectableSocket::shutdownCount() > 0;
                },
                10s);
            then("the handler gets both parts", [&] {
                require(seen == "hello world");
                require(InjectableSocket::writtenText().ends_with("echo:hello world"));
            });
        });
        for (const auto& expect :
             std::initializer_list<std::string_view>{"100-continue, 100-continue", "100-continue,100-continue", "x, 100-Continue", "100-CONTINUE ,x"}) {
            given("a client that waits for 100 Continue with 'Expect: ", expect, "'", [&] {
                send(post("/webfront/services/rpc/x", "Content-Length: 2\r\nExpect: " + std::string(expect) + "\r\n"));
                awaitRead(1);
                then("the server invites the body", [&] {
                    require(InjectableSocket::writtenText() == "HTTP/1.1 100 Continue\r\n\r\n");
                    require(calls == 0);
                });
                send("ok");
                requireEventually(
                    [] {
                        return InjectableSocket::shutdownCount() > 0;
                    },
                    10s);
                then("the handler then gets the body", [&] {
                    require(seen == "ok");
                });
            });
        }
        given("bytes beyond the announced length", [&] {
            fixture.answer(post("/webfront/services/rpc/x", "Content-Length: 2\r\n", "okGET / HTTP/1.1"));
            then("only the announced body reaches the handler", [&] {
                require(seen == "ok");
            });
        });
        given("a GET on the same route", [&] {
            const auto response = fixture.answer(get("/webfront/services/rpc/x"));
            then("it is served with an empty body", [&] {
                require(response.ends_with("echo:"));
                require(calls == 1);
            });
        });
        for (const auto& [headers, status] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {                                                   "",   "411 Length Required"},
                 {                     "Transfer-Encoding: chunked\r\n",   "501 Not Implemented"},
                 {"Content-Length: 5\r\nTransfer-Encoding: chunked\r\n",   "501 Not Implemented"},
                 {                             "Content-Length: 1x\r\n",       "400 Bad Request"},
                 {                             "Content-Length: -1\r\n",       "400 Bad Request"},
                 {        "Content-Length: 99999999999999999999999\r\n",       "400 Bad Request"},
                 {         "Content-Length: 2\r\nContent-Length: 3\r\n",       "400 Bad Request"},
                 {                           "Content-Length: 2, 3\r\n",       "400 Bad Request"},
                 {                             "Content-Length: 2,\r\n",       "400 Bad Request"},
                 {                             "Content-Length: ,2\r\n",       "400 Bad Request"},
                 {                            "Content-Length: 2 2\r\n",       "400 Bad Request"},
                 {                               "Content-Length: \r\n",       "400 Bad Request"},
                 {                                "Content-Length:\r\n",       "400 Bad Request"},
                 {                             "Content-Length: 65\r\n", "413 Payload Too Large"}
        }) {
            given("a POST with '", headers, "'", [&] {
                const auto response = fixture.answer(post("/webfront/services/rpc/x", headers));
                then("it is refused with ", status, " without reading a body or calling the handler", [&] {
                    require(response.starts_with("HTTP/1.1 " + std::string(status) + "\r\n"));
                    require(calls == 0);
                });
            });
        }
        for (const auto& headers : std::initializer_list<std::string_view>{"Content-Length:  2\r\n",
                                                                           "Content-Length: 2  \r\n",
                                                                           "Content-Length: 2, 2\r\n",
                                                                           "Content-Length: 2,2\r\n",
                                                                           "Content-Length:2\r\n",
                                                                           "Content-Length:\t2\t\r\n",
                                                                           "Content-Length: 2,\t2\r\n",
                                                                           "Content-Length: 2\r\nContent-Length: 2\r\n",
                                                                           "Content-Length: 2, 2\r\nContent-Length: 2\r\n"}) {
            given("a POST with the valid length '", headers, "'", [&] {
                const auto response = fixture.answer(post("/webfront/services/rpc/x", headers, "ok"));
                then("the body reaches the handler", [&] {
                    require(response.starts_with("HTTP/1.1 200 OK\r\n"));
                    require(seen == "ok");
                });
            });
        }
        // Where no body has to be invited, `100 Continue` is never written, even when the client asked for it.
        for (const auto& [request, why] : std::initializer_list<std::pair<std::string, std::string_view>>{
                 {post("/webfront/services/rpc/x", "Content-Length: 2\r\nExpect: 100-continue\r\n", "ok"), "the body came with the headers"},
                 {post("/webfront/services/rpc/x", "Content-Length: 65\r\nExpect: 100-continue\r\n"), "the body is too long"},
                 {post("/webfront/services/rpc/x", "Content-Length: 2\r\nExpect: x-100-continue-y\r\n", "ok"), "the value only contains the token"},
                 {"POST /webfront/services/rpc/x HTTP/1.0\r\nContent-Length: 2\r\nExpect: 100-continue\r\n\r\nok", "the client speaks HTTP/1.0"}
        }) {
            given("an Expect header when ", why, [&] {
                const auto response = fixture.answer(request);
                then("no interim answer is written", [&] {
                    require(!contains(response, "100 Continue"));
                });
            });
        }
        given("a connection that ends before the announced length", [&] {
            send(post("/webfront/services/rpc/x", "Content-Length: 10\r\n", "abc"));
            awaitRead(1);
            InjectableSocket::fail({});
            requireEventually(
                [] {
                    return InjectableSocket::shutdownCount() > 0;
                },
                10s);
            then("the connection is closed without an answer and the handler is not called", [&] {
                require(InjectableSocket::writtenText().empty());
                require(calls == 0);
            });
        });
        given("a handler that accepts without a body", [&] {
            auto other = server.addRoute("/webfront/services/status/", [](const RouteRequest& request) {
                RouteReply reply;
                reply.status        = request.query == "accepted" ? Response::accepted : Response::unauthorized;
                reply.contentLength = 0;
                return reply;
            });
            then("the status line carries the reason phrase", [&] {
                require(fixture.answer(get("/webfront/services/status/x?accepted")).starts_with("HTTP/1.1 202 Accepted\r\n"));
            });
        });
        given("a handler that refuses the credentials", [&] {
            auto other = server.addRoute("/webfront/services/status/", [](const RouteRequest&) {
                RouteReply reply;
                reply.status        = Response::unauthorized;
                reply.contentLength = 0;
                return reply;
            });
            then("the status line carries the reason phrase", [&] {
                require(fixture.answer(get("/webfront/services/status/x")).starts_with("HTTP/1.1 401 Unauthorized\r\n"));
            });
        });
        given("a route whose limit is zero", [&] {
            auto zero = server.addRoute(
                "/webfront/services/zero/",
                [&](const RouteRequest& request) {
                    return text("empty:" + std::string(request.body));
                },
                RouteOptions{.postBodyLimit = 0});
            then("an empty body is served and any other refused", [&] {
                require(fixture.answer(post("/webfront/services/zero/x", "Content-Length: 0\r\n")).ends_with("empty:"));
            });
        });
        given("a route whose limit is zero and a body of one byte", [&] {
            auto zero = server.addRoute(
                "/webfront/services/zero/",
                [&](const RouteRequest&) {
                    return text("never");
                },
                RouteOptions{.postBodyLimit = 0});
            then("it is refused with 413", [&] {
                require(fixture.answer(post("/webfront/services/zero/x", "Content-Length: 1\r\n", "a")).starts_with("HTTP/1.1 413 Payload Too Large\r\n"));
            });
        });
        given("a Content-Length folded on two lines", [&] {
            const auto response = fixture.answer(post("/webfront/services/rpc/x", "Content-Length: 1\r\n 0\r\n", "x"));
            then("the fold is a space, not a concatenated number: the length is refused", [&] {
                require(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
                require(calls == 0);
            });
        });
        given("a header folded on two lines", [&] {
            auto note = server.addRoute("/webfront/services/note/", [](const RouteRequest& request) {
                return text(request.request.getHeaderValue("X-Note").value_or("none"));
            });
            then("the handler sees one space where the line was folded", [&] {
                require(fixture.answer("GET /webfront/services/note/x HTTP/1.1\r\nHost: localhost\r\nX-Note: a\r\n   b\r\n\r\n").ends_with("a b"));
            });
        });
        given("an answer that streams from the view of a body received apart from the headers", [&] {
            auto keep = server.addRoute(
                "/webfront/services/keep/",
                [](const RouteRequest& request) {
                    RouteReply reply;
                    reply.headers.emplace_back("Content-Type", "text/plain");
                    reply.headers.emplace_back("Cache-Control", "no-store");
                    reply.contentLength = request.body.size();
                    reply.body          = [view = request.body, position = std::size_t{0}](std::span<char> out) mutable {
                        const auto count = std::min(out.size(), view.size() - position);
                        std::copy_n(view.data() + position, count, out.data());
                        position += count;
                        return count;
                    };
                    return reply;
                },
                RouteOptions{.postBodyLimit = 64});
            const std::string large(40, 'x');  // beyond the small string buffer: the body is on the heap
            send(post("/webfront/services/keep/x", "Content-Length: 40\r\n"));
            awaitRead(1);
            send(large);
            requireEventually(
                [] {
                    return InjectableSocket::shutdownCount() > 0;
                },
                10s);
            then("the body is still alive while the answer is sent", [&] {
                require(InjectableSocket::writtenText().ends_with(large));
            });
        });
        given("a PUT", [&] {
            const auto response = fixture.answer(get("/webfront/services/rpc/x", "PUT"));
            then("it is refused with the methods of the route, POST included", [&] {
                require(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
                require(contains(response, "Allow: GET, HEAD, POST\r\n"));
                require(calls == 0);
            });
        });
    }};

const Scenario pathOutside{"A path outside every route, or after unregistering, goes to the providers", [] {
                               Fixture fixture;
                               Router  server("127.0.0.1", "0");
                               auto    route = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                   return text("x");
                               });
                               given("a registration that was reset", [&] {
                                   route.reset();
                                   const auto response = fixture.answer(get("/webfront/services/files/x"));
                                   then("the request is a 404", [&] {
                                       require(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
                                   });
                               });
                           }};

const Scenario failingHandler{"A failing handler answers 500 without leaking its error", [] {
                                  Fixture fixture;
                                  Router  server("127.0.0.1", "0");
                                  given("a handler that throws", [&] {
                                      auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) -> RouteReply {
                                          throw std::runtime_error("secret detail");
                                      });
                                      const auto response = fixture.answer(get("/webfront/services/files/x"));
                                      then("the browser gets a bare 500", [&] {
                                          require(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                                          require(!contains(response, "secret detail"));
                                      });
                                  });
                                  given("a handler whose header would split the response", [&] {
                                      auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                          auto reply = text("x");
                                          reply.headers.emplace_back("X-Name", "a\r\nSet-Cookie: injected=1");
                                          return reply;
                                      });
                                      const auto response = fixture.answer(get("/webfront/services/files/x"));
                                      then("it is refused", [&] {
                                          require(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                                          require(!contains(response, "injected"));
                                      });
                                  });
                                  given("a handler setting a header the server owns", [&] {
                                      auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                          auto reply = text("x");
                                          reply.headers.emplace_back("content-length", "1");
                                          return reply;
                                      });
                                      const auto response = fixture.answer(get("/webfront/services/files/x"));
                                      then("it is refused", [&] {
                                          require(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                                      });
                                  });
                              }};

const Scenario bodyBreaks{"A body that breaks its promise closes the connection", [] {
                              Fixture fixture;
                              Router  server("127.0.0.1", "0");
                              given("a body shorter than its Content-Length", [&] {
                                  auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                      auto reply          = text("abc");
                                      reply.contentLength = 10;
                                      return reply;
                                  });
                                  const auto response = fixture.answer(get("/webfront/services/files/x"));
                                  then("the client receives a truncated body, which it detects", [&] {
                                      require(contains(response, "Content-Length: 10\r\n"));
                                      require(response.ends_with("\r\n\r\nabc"));
                                  });
                              });
                              given("a body longer than its Content-Length", [&] {
                                  auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                      auto reply          = text("abcdef");
                                      reply.contentLength = 3;
                                      return reply;
                                  });
                                  const auto response = fixture.answer(get("/webfront/services/files/x"));
                                  then("nothing past the announced length is sent", [&] {
                                      require(response.ends_with("\r\n\r\n"));
                                  });
                              });
                              given("a body that throws after a first chunk", [&] {
                                  auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                      RouteReply reply;
                                      reply.contentLength = 100;
                                      reply.body          = [first = true](std::span<char> out) mutable -> std::size_t {
                                          if (!std::exchange(first, false))
                                              throw std::runtime_error("disk gone");
                                          out[0] = 'x';
                                          return 1;
                                      };
                                      return reply;
                                  });
                                  const auto response = fixture.answer(get("/webfront/services/files/x"));
                                  then("the connection ends after what was read", [&] {
                                      require(response.ends_with("\r\n\r\nx"));
                                  });
                              });
                          }};

const Scenario slowHandler{"A slow handler does not block the server context", [] {
                               Fixture           fixture;
                               Router            server("127.0.0.1", "0");
                               std::atomic<bool> entered{false}, release{false};
                               auto              route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                   entered = true;
                                   while (!release)
                                       std::this_thread::sleep_for(1ms);
                                   return text("late");
                               });
                               send(get("/webfront/services/files/x"));
                               requireEventually(
                                   [&] {
                                       return entered.load();
                                   },
                                   10s);
                               then("the thread which sent the request has already returned, and the reply comes when the handler ends", [&] {
                                   require(InjectableSocket::shutdownCount() == 0);
                                   release = true;
                                   requireEventually(
                                       [] {
                                           return InjectableSocket::shutdownCount() > 0;
                                       },
                                       10s);
                                   require(InjectableSocket::writtenText().ends_with("late"));
                               });
                           }};

const Scenario drainingWaits{"Draining waits for a handler still running on the worker instead of reporting a stalled scope", [] {
                                 Fixture           fixture;
                                 Router            server("127.0.0.1", "0");
                                 std::atomic<bool> entered{false}, release{false};
                                 auto              route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                     entered = true;
                                     while (!release)
                                         std::this_thread::sleep_for(1ms);
                                     return text("late");
                                 });
                                 send(get("/webfront/services/files/x"));
                                 requireEventually(
                                     [&] {
                                         return entered.load();
                                     },
                                     10s);
                                 when("the server is stopped and drained while the handler has not returned", [&] {
                                     server.stop();
                                     std::jthread finishing([&] {
                                         std::this_thread::sleep_for(100ms);
                                         release = true;
                                     });
                                     then("the drain waits for the handler and returns without an error", [&] {
                                         requireNothrow([&] {
                                             server.drain();
                                         });
                                     });
                                 });
                             }};

namespace {
// Peak resident set of the process in bytes, or 0 where it cannot be read.
std::uint64_t peakResidentBytes() {
#ifdef _WIN32
    return 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
    #ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);  // bytes
    #else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;  // kilobytes
    #endif
#endif
}
struct StreamingPolicy : DefaultBuffersPolicy {
    // Keep the 128 MiB memory proof without 16384 worker round trips on instrumented ARM64 builds.
    static constexpr std::size_t emissionBufferSize = 64 * 1024;
};
}  // namespace

const Scenario largeBody{"A large body is streamed without being held", [] {
                             Fixture                                  fixture;
                             Server<RouteNet, Files, StreamingPolicy> server("127.0.0.1", "0");
                             static constexpr std::uint64_t           size         = 128ull * 1024 * 1024;
                             std::size_t                              largestChunk = 0;
                             auto                                     route        = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                 RouteReply reply;
                                 reply.contentLength = size;
                                 reply.body = [&largestChunk, produced = std::uint64_t{0}](std::span<char> out) mutable {
                                     largestChunk = std::max(largestChunk, out.size());
                                     const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), size - produced));
                                     produced += count;
                                     return count;
                                 };
                                 return reply;
                             });
                             InjectableSocket::discardWrites();
                             const auto before = peakResidentBytes();
                             send(get("/webfront/services/files/big"));
                             // Each chunk crosses to the worker thread. The limit stays below
                             // the 60 s CTest timeout of the scenarios, so that a stall is reported by the test rather than by a kill.
                             requireEventually(
                                 [] {
                                     return InjectableSocket::shutdownCount() > 0;
                                 },
                                 45s);
                             then("the peak memory of the process did not grow with the size sent", [&] {
                                 // Holding the body would add its 128 MiB to the peak; ctest runs each scenario in its own process.
                                 if (const auto after = peakResidentBytes())
                                     require(after - before < 48ull * 1024 * 1024);
                             });
                             then("every byte was sent through a buffer of the emission size", [&] {
                                 require(InjectableSocket::discardedBytes() > size);
                                 require(largestChunk == StreamingPolicy::emissionBufferSize);
                             });
                         }};

const Scenario connectionWhich{"A connection which fails while a reply is streamed stops the reading", [] {
                                   Fixture          fixture;
                                   Router           server("127.0.0.1", "0");
                                   std::atomic<int> reads{0};
                                   auto             route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                       RouteReply reply;
                                       reply.body = [&reads](std::span<char> out) {
                                           if (++reads == 2)
                                               InjectableSocket::failWrites(std::make_error_code(std::errc::connection_reset));
                                           return out.size();
                                       };
                                       return reply;
                                   });
                                   send(get("/webfront/services/files/endless"));
                                   requireEventually(
                                       [] {
                                           return InjectableSocket::shutdownCount() > 0;
                                       },
                                       10s);
                                   then("the body is not read again", [&] {
                                       const auto seen = reads.load();
                                       std::this_thread::sleep_for(20ms);
                                       require(reads == seen);
                                       require(seen <= 3);
                                   });
                               }};

const Scenario stoppingServer{"Stopping the server ends a reply being streamed", [] {
                                  Fixture fixture;
                                  Router  server("127.0.0.1", "0");
                                  auto    route = server.addRoute("/webfront/services/files/", [&](const RouteRequest&) {
                                      RouteReply reply;
                                      reply.body = [](std::span<char> out) {
                                          return out.size();
                                      };
                                      return reply;
                                  });
                                  InjectableSocket::deferWrites();
                                  send(get("/webfront/services/files/endless"));
                                  requireEventually(
                                      [] {
                                          return InjectableSocket::hasDeferredWrites();
                                      },
                                      10s);
                                  when("the server is stopped and drained while a send is pending", [&] {
                                      server.stop();
                                      then("the drain returns", [&] {
                                          requireNothrow([&] {
                                              server.drain();
                                          });
                                      });
                                  });
                              }};

namespace {

std::string pattern(std::size_t size) {
    std::string bytes;
    for (std::size_t index = 0; index < size; ++index)
        bytes.push_back(static_cast<char>((index * 7 + 3) % 251));
    return bytes;
}

// Serves, whatever the path, a file of fixed content: embedded data, or a native file when `native` names one.
struct ProvidedFile {
    static inline std::string                content;
    static inline std::vector<std::uint64_t> words;
    static inline std::filesystem::path      native;
};
struct ProvidedFS {
    explicit ProvidedFS(std::filesystem::path) {}
    std::optional<fs::File> open(std::filesystem::path) {
        if (!ProvidedFile::native.empty())
            return fs::File(std::ifstream(ProvidedFile::native, std::ios::binary));
        auto& words = ProvidedFile::words;
        words.assign((ProvidedFile::content.size() + 7) / 8, 0);
        for (std::size_t index = 0; index < ProvidedFile::content.size(); ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(ProvidedFile::content[index])) << ((7 - index % 8) * 8);
        return fs::File(words, ProvidedFile::content.size());
    }
};

}  // namespace

const Scenario filesProvider{"Files of a provider are announced by size and streamed", [] {
                                 for (const bool native : {false, true}) {
                                     given(std::string(native ? "a native file" : "an embedded file"), " of several chunks", [&] {
                                         webfront::test::TemporaryDirectory directory;
                                         ProvidedFile::content = pattern(3 * DefaultBuffersPolicy::emissionBufferSize + 11);
                                         ProvidedFile::native.clear();
                                         if (native) {
                                             ProvidedFile::native = directory.path() / "file.bin";
                                             std::ofstream(ProvidedFile::native, std::ios::binary) << ProvidedFile::content;
                                         }
                                         Fixture                      fixture;
                                         Server<RouteNet, ProvidedFS> server("127.0.0.1", "0");
                                         when("it is requested", [&] {
                                             const auto response = fixture.answer(get("/file.bin"));
                                             then("the length is announced and the whole content follows", [&] {
                                                 require(response.starts_with("HTTP/1.1 200 OK\r\n"));
                                                 require(contains(response, "Content-Length: " + std::to_string(ProvidedFile::content.size()) + "\r\n"));
                                                 require(response.ends_with("\r\n\r\n" + ProvidedFile::content));
                                             });
                                         });
                                     });
                                     given(std::string(native ? "a native file" : "an embedded file"), " requested with HEAD", [&] {
                                         webfront::test::TemporaryDirectory directory;
                                         ProvidedFile::content = pattern(20000);
                                         ProvidedFile::native.clear();
                                         if (native) {
                                             ProvidedFile::native = directory.path() / "file.bin";
                                             std::ofstream(ProvidedFile::native, std::ios::binary) << ProvidedFile::content;
                                         }
                                         Fixture                      fixture;
                                         Server<RouteNet, ProvidedFS> server("127.0.0.1", "0");
                                         const auto                   response = fixture.answer(get("/file.bin", "HEAD"));
                                         then("the length is announced and no body follows", [&] {
                                             require(contains(response, "Content-Length: 20000\r\n"));
                                             require(response.ends_with("\r\n\r\n"));
                                         });
                                     });
                                 }
                                 given("an empty file", [&] {
                                     ProvidedFile::content = "";
                                     ProvidedFile::native.clear();
                                     Fixture                      fixture;
                                     Server<RouteNet, ProvidedFS> server("127.0.0.1", "0");
                                     const auto                   response = fixture.answer(get("/file.bin"));
                                     then("it is a 200 with a zero length", [&] {
                                         require(response.starts_with("HTTP/1.1 200 OK\r\n"));
                                         require(contains(response, "Content-Length: 0\r\n"));
                                     });
                                 });
                             }};

const Scenario nativeFileKnown{"A native file known by its size reports it", [] {
                                   webfront::test::TemporaryDirectory directory;
                                   const auto                         path = directory.path() / "known.bin";
                                   std::ofstream(path, std::ios::binary) << pattern(1234);
                                   then("knownSize is the size on disk, whatever was read", [&] {
                                       fs::File file{std::ifstream(path, std::ios::binary)};
                                       require(file.knownSize() == 1234);
                                       std::array<char, 100> buffer{};
                                       require(file.read(buffer) == 100);
                                       require(file.knownSize() == 1234);
                                   });
                               }};

const Scenario failedHEAD{"Failed HEAD requests carry no body", [] {
                              Fixture fixture;
                              Router  server("127.0.0.1", "0");
                              given("a handler which throws", [&] {
                                  auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) -> RouteReply {
                                      throw std::runtime_error("no");
                                  });
                                  const auto response = fixture.answer(get("/webfront/services/files/x", "HEAD"));
                                  then("the 500 has its headers only", [&] {
                                      require(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                                      require(response.ends_with("\r\n\r\n"));
                                  });
                              });
                              given("a handler with an invalid header", [&] {
                                  auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                      auto reply = text("x");
                                      reply.headers.emplace_back("Bad Name", "v");
                                      return reply;
                                  });
                                  const auto response = fixture.answer(get("/webfront/services/files/x", "HEAD"));
                                  then("the 500 has its headers only", [&] {
                                      require(response.ends_with("\r\n\r\n"));
                                  });
                              });
                              given("a file which does not exist", [&] {
                                  const auto response = fixture.answer(get("/nothing.txt", "HEAD"));
                                  then("the 404 has its headers only", [&] {
                                      require(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
                                      require(response.ends_with("\r\n\r\n"));
                                  });
                              });
                          }};

const Scenario fileWrapped{"A file wrapped after a prefix yields the remaining bytes and their count", [] {
                               webfront::test::TemporaryDirectory directory;
                               const auto                         path = directory.path() / "prefixed.bin";
                               std::ofstream(path, std::ios::binary) << "prefix:payload";
                               std::ifstream stream(path, std::ios::binary);
                               stream.seekg(7);
                               fs::File file{std::move(stream)};
                               then("only the payload is counted and read", [&] {
                                   require(file.knownSize() == 7);
                                   std::array<char, 32> buffer{};
                                   const auto           count = file.read(buffer);
                                   require(std::string(buffer.data(), count) == "payload");
                               });
                           }};

namespace {
struct Completes {
    using receiver_concept = networking::ex::receiver_tag;
    std::promise<void>* done;
    void                set_value() noexcept {
        done->set_value();
    }
    void set_error(std::exception_ptr) noexcept {
        done->set_value();
    }
    void set_stopped() noexcept {
        done->set_value();
    }
};
}  // namespace

const Scenario blockedJob{"A blocked job does not delay the others of a worker with several threads", [] {
                              BlockingWorker     worker{2};
                              std::promise<void> release, blockedDone, quickDone;
                              auto               shared = release.get_future().share();
                              std::atomic<bool>  blocked{false};
                              auto               blockedEnded = blockedDone.get_future();
                              auto               quickEnded   = quickDone.get_future();
                              auto               slow         = networking::ex::connect(worker.run([&] {
                                  blocked = true;
                                  shared.wait();
                              }),
                                                                  Completes{&blockedDone});
                              auto               fast         = networking::ex::connect(worker.run([] {}), Completes{&quickDone});
                              // The blocked job is released whatever happens: the worker's destructor waits for it.
                              struct Release {
                                  std::promise<void>& promise;
                                  bool                done = false;
                                  void                now() {
                                      if (!std::exchange(done, true))
                                          promise.set_value();
                                  }
                                  ~Release() {
                                      now();
                                  }
                              } releaseGuard{release};
                              networking::ex::start(slow);
                              const bool started = waitUntil(
                                  [&] {
                                      return blocked.load();
                                  },
                                  10s);
                              if (started)
                                  networking::ex::start(fast);
                              const bool quickDoneInTime  = started && waitReady(quickEnded, 5s);
                              const bool slowStillBlocked = blockedEnded.wait_for(10ms) == std::future_status::timeout;
                              releaseGuard.now();
                              if (started)
                                  blockedEnded.wait();
                              if (started)
                                  quickEnded.wait();  // `fast` and `worker` outlive any use of each other, even when an assertion fails
                              then("the quick job completes while the slow one still runs", [&] {
                                  require(started);
                                  require(quickDoneInTime);
                                  require(slowStillBlocked);
                              });
                          }};

const Scenario singleThread{"A single-thread worker keeps the jobs in order", [] {
                                BlockingWorker     worker;
                                std::promise<void> firstDone, secondDone;
                                std::string        order;
                                auto               first  = networking::ex::connect(worker.run([&] {
                                    order += 'a';
                                }),
                                                                     Completes{&firstDone});
                                auto               second = networking::ex::connect(worker.run([&] {
                                    order += 'b';
                                }),
                                                                      Completes{&secondDone});
                                networking::ex::start(first);
                                networking::ex::start(second);
                                firstDone.get_future().wait();
                                secondDone.get_future().wait();
                                require(order == "ab");
                            }};

const Scenario handlerBody{"A handler or a body throwing something else than a std::exception is contained", [] {
                               Fixture fixture;
                               Router  server("127.0.0.1", "0");
                               given("a handler which throws an int", [&] {
                                   auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) -> RouteReply {
                                       throw 42;
                                   });
                                   const auto response = fixture.answer(get("/webfront/services/files/x"));
                                   then("the browser gets a 500", [&] {
                                       require(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                                   });
                               });
                               given("a body which throws an int after the headers", [&] {
                                   auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                       RouteReply reply;
                                       reply.contentLength = 10;
                                       reply.body          = [](std::span<char>) -> std::size_t {
                                           throw 42;
                                       };
                                       return reply;
                                   });
                                   const auto response = fixture.answer(get("/webfront/services/files/x"));
                                   then("the connection ends after the headers", [&] {
                                       require(response.ends_with("\r\n\r\n"));
                                   });
                               });
                           }};

const Scenario routeMatching{"Route matching sees the path as a URL path", [] {
                                 Fixture     fixture;
                                 Router      server("127.0.0.1", "0");
                                 std::string seen;
                                 auto        route = server.addRoute("/webfront/services/files/", [&](const RouteRequest& request) {
                                     seen = std::string(request.path);
                                     return text("ok");
                                 });
                                 auto        plus  = server.addRoute("/a+b/", [&](const RouteRequest& request) {
                                     seen = std::string(request.path);
                                     return text("plus");
                                 });
                                 given("a prefix with a literal plus", [&] {
                                     const auto response = fixture.answer(get("/a+b/x+y"));
                                     then("the plus stays a plus in the matching and in the path", [&] {
                                         require(response.ends_with("plus"));
                                         require(seen == "/a+b/x+y");
                                     });
                                 });
                                 given("a percent-encoded plus", [&] {
                                     const auto response = fixture.answer(get("/a%2Bb/x"));
                                     then("it matches the same route", [&] {
                                         require(response.ends_with("plus"));
                                     });
                                 });
                                 given("empty and dot segments before the prefix", [&] {
                                     for (std::string target :
                                          {"/webfront//services/files/x", "/webfront/./services/files/x", "//webfront/services//files/./x"})
                                         when(target, [&] {
                                             const auto response = fixture.answer(get(target));
                                             then("the route answers, not a file provider, with the normalized path", [&] {
                                                 require(response.ends_with("ok"));
                                                 require(seen == "/webfront/services/files/x");
                                             });
                                         });
                                 });
                             }};

const Scenario handlerMay{"A handler may answer 405 with a body of its own", [] {
                              Fixture    fixture;
                              Router     server("127.0.0.1", "0");
                              auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                  auto reply   = text("not this path");
                                  reply.status = Response::methodNotAllowed;
                                  return reply;
                              });
                              const auto response = fixture.answer(get("/webfront/services/files/x"));
                              then("its body is streamed after its headers", [&] {
                                  require(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
                                  require(response.ends_with("\r\n\r\nnot this path"));
                              });
                          }};

const Scenario headerNames{"Header names are ASCII tokens whatever the locale", [] {
                               Fixture    fixture;
                               Router     server("127.0.0.1", "0");
                               auto       route    = server.addRoute("/webfront/services/files/", [](const RouteRequest&) {
                                   auto reply = text("x");
                                   reply.headers.emplace_back("X-Caf\xC3\xA9", "v");
                                   return reply;
                               });
                               const auto response = fixture.answer(get("/webfront/services/files/x"));
                               then("a non-ASCII name is refused", [&] {
                                   require(response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                               });
                           }};
