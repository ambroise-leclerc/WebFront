#include "Mocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <http/HTTPServer.hpp>
#include <networking/NetworkingMock.hpp>

#include <list>
#include <string>

using namespace webfront;
using namespace webfront::http;
using Net = networking::NetworkingMock;

namespace {
struct ModuleFile {
    static inline std::list names{"modules/main.mjs"};
};
}  // namespace

SCENARIO("RequestHandler serves JavaScript modules") {
    std::string input{"GET /modules/main.mjs HTTP/1.1\r\nHost: localhost\r\nAccept-encoding: gzip, br, deflate\r\n\r\n"};
    Request     request;
    REQUIRE_NOTHROW(request.parseSomeData(input.cbegin(), input.cend()));

    RequestHandler<Net, MockFileSystem<ModuleFile>> handler{"."};
    auto                                            response = handler.handleRequest(request);

    REQUIRE(response.statusCode == Response::ok);
    REQUIRE(response.getHeaderValue("Content-Encoding") == "br");
    REQUIRE(response.getHeaderValue("Content-Type") == "application/javascript");
    REQUIRE(response.getHeaderValue("Content-Length") == "0");
}
