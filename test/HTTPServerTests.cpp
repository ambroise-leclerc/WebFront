#include <http/HTTPServer.hpp>
#include <networking/NetworkingMock.hpp>

#include <doctest/doctest.h>

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

    REQUIRE(Request::getMethodFromString("GET") == Request::Method::Get);
    REQUIRE(Request::getMethodFromString("HEAD") == Request::Method::Head);
    REQUIRE(Request::getMethodFromString("CONNECT") == Request::Method::Connect);
    REQUIRE(Request::getMethodFromString("DELETE") == Request::Method::Delete);
    REQUIRE(Request::getMethodFromString("OPTIONS") == Request::Method::Options);
    REQUIRE(Request::getMethodFromString("PATCH") == Request::Method::Patch);
    REQUIRE(Request::getMethodFromString("POST") == Request::Method::Post);
    REQUIRE(Request::getMethodFromString("PUT") == Request::Method::Put);
    REQUIRE(Request::getMethodFromString("TRACE") == Request::Method::Trace);
    REQUIRE(Request::getMethodFromString("test") == Request::Method::Undefined);
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
            REQUIRE(notFound.content ==
                    "<html><head><title>Not Found</title></head><body><h1>404 Not Found</h1></body></html>");
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
                REQUIRE(compare(buffers[10],
                                "<html><head><title>Bad Request</title></head><body><h1>400 Bad Request</h1></body></html>"));
            }
        }
    }
}

SCENARIO("RequestParser") {
    GIVEN("A valid HTTP request") {
        string input{"GET /hello.htm HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.tutorialspoint.com\r\nAccept-Language: en-us\r\nAccept-Encoding: gzip\r\n"
                     "Accept-Encoding: deflate\r\nConnection: Keep-Alive\r\n\r\n"};
        WHEN("parsing it") {
            Request request;
            REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
            THEN("It should fill correctly the Request structure") {
                REQUIRE(request.completed());
                REQUIRE(request.uri == "/hello.htm");
                REQUIRE(request.method == Request::Method::Get);

                auto language = request.getHeaderValue("Accept-Language");
                REQUIRE(language == "en-us");

                REQUIRE(!request.getHeaderValue("Bozo le clown"));

                auto encodings = request.getHeadersValues("Accept-Encoding");
                REQUIRE(encodings.size() == 2);

                REQUIRE(request.headersContain("Accept-Encoding", "deflate"));
                REQUIRE(request.headersContain("Accept-Encoding", "gzip"));
            }
        }
    }

    GIVEN("An invalid HTTP request") {
        string input{"GET /hello.htm HTTP/1.1\r\nUser - Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.tutorialspoint.com\r\nAccept-Language: en-us\r\nAccept-Encoding: gzip, deflate\r\nConnection: "
                     "Keep-Alive\r\n\r\n"};
        WHEN("parsing it") {
            Request request;
            REQUIRE_THROWS(request.parseSomeData(input.cbegin(), input.cend()));
        }
    }

    GIVEN("An upgrade request") {
        string input{"GET / HTTP/1.1\r\nOrigin: localhost\r\nSec-WebSocket-Protocol: WebFront_0.1\r\nSec-WebSocket-Extensions: "
                     "permessage-deflate\r\nSec-WebSocket-Key: Dh54KYbN4sDdk6ejeVPqXQ==\r\nConnection: keep-alive, "
                     "Upgrade\r\nUpgrade: websocket\r\n\r\n"};
        WHEN("parsing it") {
            Request request;
            REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
            THEN("An upgrade request should be detected") {
                REQUIRE(request.completed());
                REQUIRE(request.isUpgradeRequest("websocket"));
            }
        }
    }
}

struct MockFileSystem {
    MockFileSystem(auto){};
    struct File {
        bool isEncoded() const { return true; }
        std::string_view getEncoding() const { return "br"; }
        size_t gcount() const { return 0; }
        File& read(auto, size_t = 0) { return *this; }
    };

    static std::optional<File> open(auto) { return File{}; }
};

bool compare(auto& buffer, std::string text) {
    auto data = reinterpret_cast<const char*>(buffer.data());
    for (size_t index = 0; index < text.size(); ++index)
        if (data[index] != text[index]) return false;
    return true;
};

SCENARIO("RequestHandler") {
    GIVEN("A valid HTTP request with an unimplemented method") {
        string input{"DELETE /ressource.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
        Request request;
        REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
        REQUIRE(request.completed());
        REQUIRE(request.uri == "/ressource.txt");
        REQUIRE(request.method == Request::Method::Delete);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net, MockFileSystem> handler{"."};
            auto response = handler.handleRequest(request);

            THEN("It should respond with a 'Not Implemented' http response") {
                auto buffers = response.toBuffers<Net>();
                REQUIRE(compare(buffers[0], "HTTP/1.1 501 Not Implemented"));
            }
        }
    }

    GIVEN("A valid HTTP upgrade request to websocket protocol") {
        string input{
          "GET /chat HTTP/1.1\r\nHost: server.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
          "x3JJHMbDL1EzLkh9GBhXDw==\r\nSec-WebSocket-Protocol: chat, superchat\r\nSec-WebSocket-Version: 13\r\nOrigin: "
          "http://example.com\r\n\r\n"};
        Request request;
        REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
        REQUIRE(request.completed());
        REQUIRE(request.uri == "/chat");
        REQUIRE(request.method == Request::Method::Get);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net, MockFileSystem> handler{"."};
            auto response = handler.handleRequest(request);

            THEN("It should respond with a 'Switching Protocols' http response") {
                auto buffers = response.toBuffers<Net>();
                REQUIRE(compare(buffers[0], "HTTP/1.1 101 Switching Protocols"));
                REQUIRE(compare(buffers[1], "Upgrade"));
                REQUIRE(compare(buffers[3], "websocket"));
                REQUIRE(compare(buffers[9], "Sec-WebSocket-Accept"));
                REQUIRE(compare(buffers[11], "HSmrc0sMlYUkAGmm5OPpG2HaGWk="));
            }
        }
    }

    GIVEN("A HTTP HEAD request") {
        string input{"HEAD /file.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
        Request request;
        REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
        REQUIRE(request.completed());
        REQUIRE(request.uri == "/file.txt");
        REQUIRE(request.method == Request::Method::Head);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net, MockFileSystem> handler{"."};
            auto response = handler.handleRequest(request);

            THEN("It should find the file") { REQUIRE(response.statusCode == Response::ok); }
        }
        
    }

    GIVEN("A HTTP HEAD request with an empty URL") {
        string input{"HEAD HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
                
        Request request;
        REQUIRE_THROWS_AS(request.parseSomeData(input.cbegin(), input.cend()), BadRequestException);
       
    }

    GIVEN("A HTTP HEAD request received asynchronously") {
        string chunk1{"HEAD /file.txt HTTP/1.1\r\nUser-Agent:"};
        string chunk2{" Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "};
        string chunk3{"www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
        Request request;
        REQUIRE_NOTHROW(request.parseSomeData(chunk1.cbegin(), chunk1.cend()));
        REQUIRE(!request.completed());
        REQUIRE_NOTHROW(request.parseSomeData(chunk2.cbegin(), chunk2.cend()));
        REQUIRE(!request.completed());
        REQUIRE_NOTHROW(request.parseSomeData(chunk3.cbegin(), chunk3.cend()));
        REQUIRE(request.completed());
        REQUIRE(request.uri == "/file.txt");
        REQUIRE(request.method == Request::Method::Head);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net, MockFileSystem> handler{"."};
            auto response = handler.handleRequest(request);

            THEN("It should find the file") { REQUIRE(response.statusCode == Response::ok); }
        }
        
    }
}

SCENARIO("RequestHandler on a HTTP GET") {
    GIVEN("A valid HTTP GET request on a compressed file with no supported encoding") {
        string input{"GET /compressed.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\n\r\n"};
        Request request;
        REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
        REQUIRE(request.completed());
        REQUIRE(request.uri == "/compressed.txt");
        REQUIRE(request.method == Request::Method::Get);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net, MockFileSystem> handler{"."};
            auto response = handler.handleRequest(request);

            THEN("It should respond with a 'Variant Also Negotiates' http response") {
                auto buffers = response.toBuffers<Net>();
                REQUIRE(compare(buffers[0], "HTTP/1.1 506 Variant Also Negotiates"));
            }
        }
    }

    GIVEN("A valid HTTP GET request on a compressed file with supported encoding") {
        string input{"GET /compressed.txt HTTP/1.1\r\nUser-Agent: Mozilla / 4.0 (compatible; MSIE5.01; Windows NT)\r\nHost: "
                     "www.bernardlehacker.com\r\nConnection: Keep-Alive\r\nAccept-encoding: gzip, br, deflate\r\n\r\n"};
        Request request;
        REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));
        REQUIRE(request.completed());
        REQUIRE(request.uri == "/compressed.txt");
        REQUIRE(request.method == Request::Method::Get);
        WHEN("A RequestHandler process it") {
            RequestHandler<Net, MockFileSystem> handler{"."};
            auto response = handler.handleRequest(request);
            THEN("It should respond ok with a brotli encoded (empty) file") {
                REQUIRE(response.statusCode == Response::ok);
                REQUIRE(response.getHeaderValue("Content-Encoding") == "br");
                REQUIRE(response.getHeaderValue("Content-Type") == "text/plain");
                REQUIRE(response.getHeaderValue("Content-Length") == "0");
            }
        }
    }
}