// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-01-16
//
// WebFront — tests/unit
// Role: The HTTP server: requests, headers, files, resources and error replies.
// Thread: bridge-io | Alloc: unrestricted
// LLM: uses the networking mock

#include "InjectableNetworking.hpp"
#include "Mocks.hpp"
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;
using namespace webfront::http;
using namespace std;
using Net = networking::NetworkingMock;

const Scenario hTTPRequest{"HTTP Request", [] {
                               given("A request with % encoded characters", [&] {
                                   Request request;
                                   request.uri = "http://aaa.bbb.ccc/bozo%20le%20clown";

                                   when("decoding url", [&] {
                                       then("% are expanded and decoded", [&] {
                                           require(uri::decode(request.uri) == "http://aaa.bbb.ccc/bozo le clown");
                                       });
                                   });
                               });

                               require(Request::getMethodFromString("GET") == Request::Method::Get);
                               require(Request::getMethodFromString("HEAD") == Request::Method::Head);
                               require(Request::getMethodFromString("CONNECT") == Request::Method::Connect);
                               require(Request::getMethodFromString("DELETE") == Request::Method::Delete);
                               require(Request::getMethodFromString("OPTIONS") == Request::Method::Options);
                               require(Request::getMethodFromString("PATCH") == Request::Method::Patch);
                               require(Request::getMethodFromString("POST") == Request::Method::Post);
                               require(Request::getMethodFromString("PUT") == Request::Method::Put);
                               require(Request::getMethodFromString("TRACE") == Request::Method::Trace);
                               require(Request::getMethodFromString("test") == Request::Method::Undefined);
                           }};

const Scenario hTTPResponse{"HTTP Response", [] {
                                given("Ok Response", [&] {
                                    auto ok = Response::getStatusResponse(Response::ok);
                                    then("Fields should be", [&] {
                                        require(ok.content == "<html><head><title>OK</title></head><body><h1>200 OK</h1></body></html>");
                                        require(ok.headers[0].name == "Content-Length");
                                        require(ok.headers[1].name == "Content-Type");
                                        require(ok.headers[0].value == "71");
                                        require(ok.headers[1].value == "text/html");
                                    });
                                });

                                given("notFound Response", [&] {
                                    auto notFound = Response::getStatusResponse(Response::notFound);
                                    then("Fields should be", [&] {
                                        require(notFound.content == "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>");
                                        require(notFound.headers[0].name == "Content-Length");
                                        require(notFound.headers[1].name == "Content-Type");
                                        require(notFound.headers[0].value == "85");
                                        require(notFound.headers[1].value == "text/html");
                                    });
                                });

                                given("badRequest Response", [&] {
                                    auto bad = Response::getStatusResponse(Response::badRequest);
                                    when("extracting buffers", [&] {
                                        auto buffers = bad.toBuffers<Net>();
                                        then("buffer contents should be", [&] {
                                            require(buffers.size() == 12);
                                            using Buffer = decltype(buffers[0]);
                                            auto compare = [](Buffer b, std::string s) {
                                                auto data = reinterpret_cast<const char*>(b.data());
                                                for (size_t index = 0; index < s.size(); ++index)
                                                    if (data[index] != s[index])
                                                        return false;
                                                return true;
                                            };
                                            require(compare(buffers[0], "HTTP/1.1 400 Bad Request"));
                                            require(compare(buffers[1], "Content-Length"));
                                            require(compare(buffers[2], ": "));
                                            require(compare(buffers[3], "89"));
                                            require(compare(buffers[4], "\r\n"));
                                            require(compare(buffers[5], "Content-Type"));
                                            require(compare(buffers[6], ": "));
                                            require(compare(buffers[7], "text/html"));
                                            require(compare(buffers[8], "\r\n"));
                                            require(compare(buffers[9], "Connection: close\r\n"));
                                            require(compare(buffers[10], "\r\n"));
                                            require(compare(buffers[11],
                                                            "<html><head><title>Bad Request</title></head><body><h1>400 Bad Request</h1></body></html>"));
                                        });
                                    });
                                });
                            }};

const Scenario pendingHTTP{"Pending HTTP response buffers remain independent and announce connection closure", [] {
                               const auto text = [](const auto& buffers) {
                                   std::string result;
                                   for (const auto& buffer : buffers)
                                       result.append(static_cast<const char*>(buffer.data()), buffer.size());
                                   return result;
                               };
                               auto first         = Response::getStatusResponse(Response::internalServerError);
                               auto firstBuffers  = first.toBuffers<Net>();
                               auto second        = Response::getStatusResponse(Response::ok);
                               auto secondBuffers = second.toBuffers<Net>();
                               then("another pending send cannot replace the first status line", [&] {
                                   require(text(firstBuffers).starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                                   require(text(secondBuffers).starts_with("HTTP/1.1 200 OK\r\n"));
                               });
                               then("ordinary responses tell clients that the server will close the connection", [&] {
                                   require(text(firstBuffers).find("\r\nConnection: close\r\n") != std::string::npos);
                                   require(text(secondBuffers).find("\r\nConnection: close\r\n") != std::string::npos);
                               });
                               given("a WebSocket upgrade", [&] {
                                   Response upgrade;
                                   upgrade.statusCode = Response::switchingProtocols;
                                   upgrade.headers.emplace_back("Connection", "Upgrade");
                                   const auto response = text(upgrade.toBuffers<Net>());
                                   require(response.find("\r\nConnection: Upgrade\r\n") != std::string::npos);
                                   require(response.find("Connection: close") == std::string::npos);
                                   require(text(firstBuffers).starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
                               });
                           }};

const Scenario requestParser{"RequestParser", [] {
                                 given("A valid HTTP request", [&] {
                                     string input{"GET /hello.htm HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                  "www.tutorialspoint.com\r\nAccept-Language: en-us\r\nAccept-Encoding: gzip\r\n"
                                                  "Accept-Encoding: deflate\r\nConnection: Keep-Alive\r\n\r\n"};
                                     when("parsing it", [&] {
                                         Request request;
                                         requireNothrow([&] {
                                             request.parseSomeData(input.cbegin(), input.cend());
                                         });
                                         then("It should fill correctly the Request structure", [&] {
                                             require(request.completed());
                                             require(request.uri == "/hello.htm");
                                             require(request.method == Request::Method::Get);

                                             auto language = request.getHeaderValue("Accept-Language");
                                             require(language == "en-us");

                                             require(!request.getHeaderValue("Bozo le clown"));

                                             auto encodings = request.getHeadersValues("Accept-Encoding");
                                             require(encodings.size() == 2);

                                             require(request.headersContain("Accept-Encoding", "deflate"));
                                             require(request.headersContain("Accept-Encoding", "gzip"));
                                         });
                                     });
                                 });

                                 given("An invalid HTTP request", [&] {
                                     string input{"GET /hello.htm HTTP/1.1\r\nUser - Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                  "www.tutorialspoint.com\r\nAccept-Language: en-us\r\nAccept-Encoding: gzip, deflate\r\nConnection: "
                                                  "Keep-Alive\r\n\r\n"};
                                     when("parsing it", [&] {
                                         Request request;
                                         requireThrows([&] {
                                             request.parseSomeData(input.cbegin(), input.cend());
                                         });
                                     });
                                 });

                                 given("An upgrade request", [&] {
                                     string input{"GET / HTTP/1.1\r\nOrigin: localhost\r\nSec-WebSocket-Protocol: WebFront_0.1\r\nSec-WebSocket-Extensions: "
                                                  "permessage-deflate\r\nSec-WebSocket-Key: Dh54KYbN4sDdk6ejeVPqXQ==\r\nConnection: keep-alive, "
                                                  "Upgrade\r\nUpgrade: websocket\r\n\r\n"};
                                     when("parsing it", [&] {
                                         Request request;
                                         requireNothrow([&] {
                                             request.parseSomeData(input.cbegin(), input.cend());
                                         });
                                         then("An upgrade request should be detected", [&] {
                                             require(request.completed());
                                             require(request.isUpgradeRequest("websocket"));
                                         });
                                     });
                                 });
                             }};

struct ResourceAndFile {
    static inline list names{"ressource.txt", "file.txt", "compressed.txt"};
};

bool compare(auto& buffer, std::string text) {
    auto data = reinterpret_cast<const char*>(buffer.data());
    for (size_t index = 0; index < text.size(); ++index)
        if (data[index] != text[index])
            return false;
    return true;
};

const Scenario requestParsing{"Request parsing", [] {
                                  given("A valid HTTP request with an unimplemented method", [&] {
                                      string  input{"DELETE /ressource.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                    "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
                                      Request request;
                                      requireNothrow([&] {
                                          request.parseSomeData(input.cbegin(), input.cend());
                                      });
                                      require(request.completed());
                                      require(request.uri == "/ressource.txt");
                                      require(request.method == Request::Method::Delete);
                                      when("A RequestHandler process it", [&] {
                                          RequestHandler<Net, MockFileSystem<ResourceAndFile>> handler{"."};
                                          auto                                                 response = handler.handleRequest(request);

                                          then("It should respond with a 'Not Implemented' http response", [&] {
                                              auto buffers = response.toBuffers<Net>();
                                              require(compare(buffers[0], "HTTP/1.1 501 Not Implemented"));
                                          });
                                      });
                                  });

                                  given("A valid HTTP upgrade request to websocket protocol", [&] {
                                      string input{
                                          "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
                                          "x3JJHMbDL1EzLkh9GBhXDw==\r\nSec-WebSocket-Protocol: chat, superchat\r\nSec-WebSocket-Version: 13\r\nOrigin: "
                                          "http://example.com\r\n\r\n"};
                                      Request request;
                                      requireNothrow([&] {
                                          request.parseSomeData(input.cbegin(), input.cend());
                                      });
                                      require(request.completed());
                                      require(request.uri == "/chat");
                                      require(request.method == Request::Method::Get);
                                      when("A RequestHandler process it", [&] {
                                          RequestHandler<Net, MockFileSystem<>> handler{".", detail::BoundContentPolicy{"http://example.com"}};
                                          auto                                  response = handler.handleRequest(request);

                                          then("It should respond with a 'Switching Protocols' http response", [&] {
                                              auto buffers = response.toBuffers<Net>();
                                              require(compare(buffers[0], "HTTP/1.1 101 Switching Protocols"));
                                              require(compare(buffers[1], "Upgrade"));
                                              require(compare(buffers[3], "websocket"));
                                              require(compare(buffers[9], "Sec-WebSocket-Accept"));
                                              require(compare(buffers[11], "HSmrc0sMlYUkAGmm5OPpG2HaGWk="));
                                          });
                                      });
                                  });

                                  given("A HTTP HEAD request", [&] {
                                      string  input{"HEAD /file.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                    "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\nAccept-Encoding: br\r\n\r\n"};
                                      Request request;
                                      requireNothrow([&] {
                                          request.parseSomeData(input.cbegin(), input.cend());
                                      });
                                      require(request.completed());
                                      require(request.uri == "/file.txt");
                                      require(request.method == Request::Method::Head);
                                      when("A RequestHandler process it", [&] {
                                          RequestHandler<Net, MockFileSystem<ResourceAndFile>> handler{"."};
                                          auto                                                 response = handler.handleRequest(request);

                                          then("It should find the file", [&] {
                                              require(response.statusCode == Response::ok);
                                          });
                                      });
                                  });

                                  given("A HTTP HEAD request with an empty URL", [&] {
                                      string input{"HEAD HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                   "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};

                                      Request request;
                                      requireThrows<BadRequestException>([&] {
                                          request.parseSomeData(input.cbegin(), input.cend());
                                      });
                                  });
                              }};

const Scenario headerWhitespace{
    "HTTP header values accept optional whitespace and empty values",
    [] {
        given("headers split across individual receives", [] {
            const std::string input = "GET / HTTP/1.1\r\nContent-Length:5\r\nX-Tab:\t \talpha\tbeta \t\r\nX-Empty:\r\nX-Blank: \t\r\n\r\n";
            Request           request;
            for (const char& byte : input)
                request.parseSomeData(&byte, &byte + 1);
            require(request.completed());
            require(request.getHeaderValue("Content-Length") == "5");
            require(request.getHeaderValue("X-Tab") == "alpha\tbeta \t");
            require(request.getHeaderValue("X-Empty") == "");
            require(request.getHeaderValue("X-Blank") == "");
        });
        given("forbidden control characters in a field value", [] {
            for (const char control : {'\0', '\1', '\n', '\x7f'}) {
                std::string input = "GET / HTTP/1.1\r\nX-Test: value";
                input            += control;
                input            += "more\r\n\r\n";
                Request request;
                requireThrows<BadRequestException>([&] {
                    request.parseSomeData(input.begin(), input.end());
                });
            }
            const std::string input = "GET / HTTP/1.1\r\nX-Test: value\rmore\r\n\r\n";
            Request           request;
            requireThrows<BadRequestException>([&] {
                request.parseSomeData(input.begin(), input.end());
            });
        });
        given("a forbidden control character before a field value", [] {
            for (const std::string prefix : {"X-Test:", "X-Test: "}) {
                for (const char control : {'\0', '\1', '\n', '\x7f'}) {
                    std::string input = "GET / HTTP/1.1\r\n" + prefix;
                    input            += control;
                    input            += "value\r\n\r\n";
                    Request request;
                    requireThrows<BadRequestException>([&] {
                        request.parseSomeData(input.begin(), input.end());
                    });
                }
            }
        });
    }};

const Scenario asynchronousRequest{"Asynchronous Request parsing", [] {
                                       given("A HTTP HEAD request received asynchronously", [&] {
                                           string  chunk1{"HEAD /file.txt HTTP/1.1\r\nUser-Agent:"};
                                           string  chunk2{" Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "};
                                           string  chunk3{"www.bernardlehacker.com\r\nConnection: Keep-Alive\r\nAccept-Encoding: br\r\n\r\n"};
                                           Request request;
                                           requireNothrow([&] {
                                               request.parseSomeData(chunk1.cbegin(), chunk1.cend());
                                           });
                                           require(!request.completed());
                                           requireNothrow([&] {
                                               request.parseSomeData(chunk2.cbegin(), chunk2.cend());
                                           });
                                           require(!request.completed());
                                           requireNothrow([&] {
                                               request.parseSomeData(chunk3.cbegin(), chunk3.cend());
                                           });
                                           require(request.completed());
                                           require(request.uri == "/file.txt");
                                           require(request.method == Request::Method::Head);
                                           when("A RequestHandler process it", [&] {
                                               RequestHandler<Net, MockFileSystem<ResourceAndFile>> handler{"."};
                                               auto                                                 response = handler.handleRequest(request);

                                               then("It should find the file", [&] {
                                                   require(response.statusCode == Response::ok);
                                               });
                                           });
                                       });
                                   }};

const Scenario requestHandlerHTTP{"RequestHandler on a HTTP GET", [] {
                                      given("A valid HTTP GET request on a compressed file with no supported encoding", [&] {
                                          string  input{"GET /compressed.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                        "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
                                          Request request;
                                          requireNothrow([&] {
                                              request.parseSomeData(input.cbegin(), input.cend());
                                          });
                                          require(request.completed());
                                          require(request.uri == "/compressed.txt");
                                          require(request.method == Request::Method::Get);
                                          when("A RequestHandler process it", [&] {
                                              RequestHandler<Net, MockFileSystem<ResourceAndFile>> handler{"."};
                                              auto                                                 response = handler.handleRequest(request);

                                              then("It should reject an unavailable custom encoding", [&] {
                                                  auto buffers = response.toBuffers<Net>();
                                                  require(compare(buffers[0], "HTTP/1.1 406 Not Acceptable"));
                                              });
                                          });
                                      });

                                      given("A valid HTTP GET request on a compressed file with supported encoding", [&] {
                                          string  input{"GET /compressed.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                                                        "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\nAccept-encoding: gzip, br, deflate\r\n\r\n"};
                                          Request request;
                                          requireNothrow([&] {
                                              request.parseSomeData(input.cbegin(), input.cend());
                                          });
                                          require(request.completed());
                                          require(request.uri == "/compressed.txt");
                                          require(request.method == Request::Method::Get);
                                          when("A RequestHandler process it", [&] {
                                              RequestHandler<Net, MockFileSystem<ResourceAndFile>> handler{"."};
                                              auto                                                 response = handler.handleRequest(request);
                                              then("It should respond ok with a brotli encoded (empty) file", [&] {
                                                  require(response.statusCode == Response::ok);
                                                  require(response.getHeaderValue("Content-Encoding") == "br");
                                                  require(response.getHeaderValue("Content-Type") == "text/plain");
                                                  require(response.getHeaderValue("Content-Length") == "0");
                                              });
                                          });
                                      });
                                  }};

namespace {

// Accepts one connection which asks for a WebSocket upgrade; the end of the response write is held.
// The first accept fails if failFirstAccept is set, and the accepts after the connection are cancelled.
struct UpgradingNet : networking::NetworkingMock {
    static inline int                                         accepts         = 0;
    static inline bool                                        failFirstAccept = false;
    static inline webfront::testing::ManualSender::Completion heldSend;

    static void reset(bool failFirst) {
        accepts         = 0;
        failFirstAccept = failFirst;
        heldSend        = {};
    }

    static auto Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            const int number = ++accepts;
            if (failFirstAccept && number == 1)
                throw std::system_error(make_error_code(errc::connection_aborted));
            if (number > (failFirstAccept ? 2 : 1))
                throw std::system_error(make_error_code(errc::operation_canceled));
            return Socket{};
        });
    }

    static auto Receive(Socket&, MutableBuffer buffer) {
        return networking::ex::then(networking::ex::just(), [buffer] {
            static constexpr string_view request{"GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"};
            copy(request.begin(), request.end(), static_cast<char*>(buffer.data()));
            return request.size();
        });
    }

    static webfront::testing::ManualSender Send(Socket&, const auto&) {
        return webfront::testing::ManualSender([](webfront::testing::ManualSender::Completion completion) {
            heldSend = std::move(completion);
        });
    }
};

}  // namespace

const Scenario stoppedServer{"A stopped server hands no connection over", [] {
                                 given("A connection asking for an upgrade, whose response write is still in progress", [&] {
                                     int upgrades = 0;
                                     UpgradingNet::reset(false);
                                     {
                                         http::Server<UpgradingNet, fs::DefaultFS> server("127.0.0.1", "0");
                                         server.onUpgrade([&upgrades](UpgradingNet::Socket&&) {
                                             ++upgrades;
                                         });
                                         require(UpgradingNet::heldSend.complete);

                                         when("the server is stopped before the response is written", [&] {
                                             server.stop();
                                             UpgradingNet::heldSend.complete({}, 0);

                                             then("the connection is closed instead of being upgraded", [&] {
                                                 require(upgrades == 0);
                                             });
                                         });
                                     }
                                 });
                             }};

const Scenario failedAccept{"A failed accept does not stop the server from accepting", [] {
                                given("A first accept which fails, then a connection", [&] {
                                    UpgradingNet::reset(true);
                                    http::Server<UpgradingNet, fs::DefaultFS> server("127.0.0.1", "0");

                                    then("the connection is served", [&] {
                                        require(UpgradingNet::heldSend.complete);
                                    });
                                });
                            }};

// Serves "index.html", "app.js" and "a?b.txt", and records every path it is asked to open.
struct RecordingFS {
    static inline vector<string> opened;

    explicit RecordingFS(filesystem::path) { }

    static optional<fs::File> open(filesystem::path file) {
        opened.push_back(file.generic_string());
        static constexpr array<string_view, 3> names{"index.html", "app.js", "a?b.txt"};
        if (ranges::find(names, file.generic_string()) == names.end())
            return {};
        return fs::File{ifstream{}};
    }
};

const Scenario requestHandlerIgnores{"RequestHandler ignores the query string of a request", [] {
                                         RecordingFS::opened.clear();
                                         RequestHandler<Net, RecordingFS> handler{"."};
                                         const auto                       get = [&](string uri, Request::Method method = Request::Method::Get) {
                                             Request request;
                                             request.method = method;
                                             request.uri    = std::move(uri);
                                             return handler.handleRequest(request).statusCode;
                                         };

                                         given("A GET request with a query", [&] {
                                             then("The file is looked up without it", [&] {
                                                 require(get("/app.js?v=2&debug=1") == Response::ok);
                                                 require(RecordingFS::opened == vector<string>{"app.js"});
                                             });
                                         });
                                         given("A query on a file that does not exist", [&] {
                                             then("The request fails with not found", [&] {
                                                 require(get("/missing.js?v=2") == Response::notFound);
                                                 require(RecordingFS::opened == vector<string>{"missing.js"});
                                             });
                                         });
                                         given("A query on the root", [&] {
                                             then("The default page is served", [&] {
                                                 require(get("/?x=1") == Response::ok);
                                                 require(RecordingFS::opened == vector<string>{"index.html"});
                                             });
                                         });
                                         given("A HEAD request with a query", [&] {
                                             then("The file is found", [&] {
                                                 require(get("/app.js?v=2", Request::Method::Head) == Response::ok);
                                             });
                                         });
                                         given("A percent-encoded question mark", [&] {
                                             then("It is part of the file name, not a query", [&] {
                                                 require(get("/a%3Fb.txt") == Response::ok);
                                                 require(RecordingFS::opened == vector<string>{"a?b.txt"});
                                             });
                                         });
                                         given("Encoded dots in the path, or a question mark in the query", [&] {
                                             then("The '..' check applies to the decoded path only", [&] {
                                                 require(get("/%2E%2E/secret.txt") == Response::badRequest);
                                                 require(get("/app.js?path=../x") == Response::ok);
                                             });
                                         });
                                     }};

const Scenario hTTPVersion{"HTTP version digits cannot overflow their integer fields", [] {
                               for (const std::string version : {"99999999999999999999.1", "1.99999999999999999999"}) {
                                   const auto input = "GET / HTTP/" + version + "\r\n\r\n";
                                   Request    request;
                                   requireThrows<BadRequestException>([&] {
                                       request.parseSomeData(input.begin(), input.end());
                                   });
                               }
                           }};

const Scenario unknownHTTP{"Unknown HTTP methods have a safe display name", [] {
                               const std::string input = "CUSTOM / HTTP/1.1\r\n\r\n";
                               Request           request;
                               require(request.parseSomeData(input.begin(), input.end()));
                               require(request.getMethodName() == "Undefined");
                           }};

const Scenario hTTPRequests{"HTTP requests keep method parsing state across interleaved receives", [] {
                                given("two connections whose method names arrive in separate reads", [&] {
                                    Request           first;
                                    Request           second;
                                    const std::string firstStart  = "G";
                                    const std::string secondStart = "PO";
                                    require(!first.parseSomeData(firstStart.begin(), firstStart.end()));
                                    require(!second.parseSomeData(secondStart.begin(), secondStart.end()));

                                    when("the first request completes before the second", [&] {
                                        const std::string firstRest  = "ET /first HTTP/1.1\r\n\r\n";
                                        const std::string secondRest = "ST /second HTTP/1.1\r\n\r\n";
                                        require(first.parseSomeData(firstRest.begin(), firstRest.end()));
                                        require(second.parseSomeData(secondRest.begin(), secondRest.end()));

                                        then("the first request retains its method and URI", [&] {
                                            check(first.method == Request::Method::Get);
                                            check(first.uri == "/first");
                                        });
                                        andThen("the second request retains its method and URI", [&] {
                                            check(second.method == Request::Method::Post);
                                            check(second.uri == "/second");
                                        });
                                    });
                                });
                            }};
