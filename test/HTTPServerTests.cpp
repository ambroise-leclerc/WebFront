#include <http/HTTPServer.hpp>
#include <networking/NetworkingMock.hpp>

#include <catch2/catch.hpp>

#include <string>
#include <string_view>

using namespace webfront;
using namespace webfront::http;
using namespace std;
using Net = networking::NetworkingMock;

SCENARIO("HTTP Request") {
    GIVEN("A request with % encoded characters") {
        Request request;
        request.uri = "http://aaa.bbb.ccc/bozo%20le%20clown";

        WHEN("decoding url") {
            THEN("% are expanded and decoded") { REQUIRE(uri::decode(request.uri) == "http://aaa.bbb.ccc/bozo le clown"); }
        }
    }
}

SCENARIO("HTTP Response") {
    GIVEN("Ok Response") {
        auto ok = Response::getStatusResponse(Response::ok);
        THEN("Fields should be") {
            REQUIRE(ok.content == "<html><head><title>OK</title></head><body><h1>200 OK</h1></body></html>");
            REQUIRE(ok.headers[0].name == "Content-Length");
            REQUIRE(ok.headers[1].name == "Content-Type");
            REQUIRE(ok.headers[0].value == "71");
            REQUIRE(ok.headers[1].value == "text/html");
        }
    }

    GIVEN("notFound Response") {
        auto notFound = Response::getStatusResponse(Response::notFound);
        THEN("Fields should be") {
            REQUIRE(notFound.content == "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>");
            REQUIRE(notFound.headers[0].name == "Content-Length");
            REQUIRE(notFound.headers[1].name == "Content-Type");
            REQUIRE(notFound.headers[0].value == "85");
            REQUIRE(notFound.headers[1].value == "text/html");
        }
    }

    GIVEN("badRequest Response") {
        auto bad = Response::getStatusResponse(Response::badRequest);
        WHEN("extracting buffers") {
            auto buffers = bad.toBuffers<Net>();
            THEN("buffer contents should be") {
                REQUIRE(buffers.size() == 11);
                using Buffer = decltype(buffers[0]);
                auto compare = [](Buffer b, std::string s) {
                    auto data = reinterpret_cast<const char*>(b.data());
                    for (size_t index = 0; index < s.size(); ++index)
                        if (data[index] != s[index]) return false;
                    return true;
                };
                REQUIRE(compare(buffers[0], "HTTP/1.1 400 Bad Request"));
                REQUIRE(compare(buffers[1], "Content-Length"));
                REQUIRE(compare(buffers[2], ": "));
                REQUIRE(compare(buffers[3], "89"));
                REQUIRE(compare(buffers[4], "\r\n"));
                REQUIRE(compare(buffers[5], "Content-Type"));
                REQUIRE(compare(buffers[6], ": "));
                REQUIRE(compare(buffers[7], "text/html"));
                REQUIRE(compare(buffers[8], "\r\n"));
                REQUIRE(compare(buffers[9], "\r\n"));
                REQUIRE(compare(buffers[10], "<html><head><title>Bad Request</title></head><body><h1>400 Bad Request</h1></body></html>"));
            }
        }
    }
}

SCENARIO("RequestParser") {
    GIVEN("A valid HTTP request") {
        RequestParser parser;

        string input{"GET /hello.htm HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.tutorialspoint.com\r\nAccept-Language: en-us\r\nAccept-Encoding: gzip, deflate\r\nConnection: Keep-Alive\r\n\r\n"};
        WHEN("parsing it") {
            optional<Request> request;
            REQUIRE_NOTHROW(request = parser.parse(input.cbegin(), input.cend()));
            THEN("It should fill correctly the Request structure") {
                REQUIRE(request);
                REQUIRE(request.value().uri == "/hello.htm");
                REQUIRE(request.value().method == Request::Method::Get);
            }
        }
    }

    GIVEN("An invalid HTTP request") {
        RequestParser parser;

        string input{"GET /hello.htm HTTP/1.1\r\nUser - Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.tutorialspoint.com\r\nAccept-Language: en-us\r\nAccept-Encoding: gzip, deflate\r\nConnection: Keep-Alive\r\n\r\n"};
        WHEN("parsing it") {
            optional<Request> request;
            REQUIRE_THROWS(request = parser.parse(input.cbegin(), input.cend()));
        }
    }

    GIVEN("An upgrade request") {
        RequestParser parser;

        string input{"GET / HTTP/1.1\r\nOrigin: localhost\r\nSec-WebSocket-Protocol: WebFront_0.1\r\nSec-WebSocket-Extensions: "
                     "permessage-deflate\r\nSec-WebSocket-Key: Dh54KYbN4sDdk6ejeVPqXQ==\r\nConnection: keep-alive, Upgrade\r\nUpgrade: websocket\r\n\r\n"};
        WHEN("parsing it") {
            optional<Request> request;
            REQUIRE_NOTHROW(request = parser.parse(input.cbegin(), input.cend()));
            THEN("An upgrade request should be detected") {
                REQUIRE(request);
                REQUIRE(request.value().isUpgradeRequest("websocket"));
            }
        }
    }
}

SCENARIO("RequestHandler") {
    GIVEN("A valid HTTP request with an unimplemented method") {
        RequestParser parser;

        string input{"DELETE /ressource.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
        optional<Request> request;
        REQUIRE_NOTHROW(request = parser.parse(input.cbegin(), input.cend()));
        REQUIRE(request);
        REQUIRE(request.value().uri == "/ressource.txt");
        REQUIRE(request.value().method == Request::Method::Delete);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net> handler{"."};
            auto response = handler.handleRequest(request.value());

            THEN("It should respond with a 'Not Implemented' http response") {
                auto buffers = response.toBuffers<Net>();
                using Buffer = decltype(buffers[0]);
                auto compare = [](Buffer& b, std::string s) {
                    auto data = reinterpret_cast<const char*>(b.data());
                    for (size_t index = 0; index < s.size(); ++index)
                        if (data[index] != s[index]) return false;
                    return true;
                };

                REQUIRE(compare(buffers[0], "HTTP/1.1 501 Not Implemented"));
            }
        }
    }

    GIVEN("A valid HTTP upgrade request to websocket protocol") {
        RequestParser parser;

        string input{"GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
                     "x3JJHMbDL1EzLkh9GBhXDw==\r\nSec-WebSocket-Protocol: chat, superchat\r\nSec-WebSocket-Version: 13\r\nOrigin: http://example.com\r\n\r\n"};
        optional<Request> request;
        REQUIRE_NOTHROW(request = parser.parse(input.cbegin(), input.cend()));
        REQUIRE(request);
        REQUIRE(request.value().uri == "/chat");
        REQUIRE(request.value().method == Request::Method::Get);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net> handler{"."};
            auto response = handler.handleRequest(request.value());

            THEN("It should respond with a 'Switching Protocols' http response") {
                auto buffers = response.toBuffers<Net>();
                using Buffer = decltype(buffers[0]);
                auto compare = [](Buffer& b, std::string s) {
                    auto data = reinterpret_cast<const char*>(b.data());
                    for (size_t index = 0; index < s.size(); ++index)
                        if (data[index] != s[index]) return false;
                    return true;
                };

                REQUIRE(compare(buffers[0], "HTTP/1.1 101 Switching Protocols"));
                REQUIRE(compare(buffers[1], "Upgrade"));
                REQUIRE(compare(buffers[3], "websocket"));
                REQUIRE(compare(buffers[9], "Sec-WebSocket-Accept"));
                REQUIRE(compare(buffers[11], "HSmrc0sMlYUkAGmm5OPpG2HaGWk="));
            }
        }
    }
}
