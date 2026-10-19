#include "Mocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <http/HTTPServer.hpp>
#include <networking/NetworkingMock.hpp>
#include <system/BabelFS.hpp>
#include <system/JasmineFS.hpp>
#include <system/ReactFS.hpp>

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

// The shared gzip assets already exist on develop. Keep this HTTP negotiation
// guard for WebKitGTK, which does not advertise Brotli over loopback HTTP.

SCENARIO("Embedded browser dependencies support gzip-only HTTP clients") {
    using Filesystem = fs::Multi<fs::JasmineFS, fs::ReactFS, fs::BabelFS>;
    RequestHandler<Net, Filesystem> handler{"."};
    for (const auto path : {"jasmine/4.6.0/jasmine.js",
                            "react@18/umd/react.production.min.js",
                            "react-dom@18/umd/react-dom.production.min.js",
                            "babel-standalone@7.22.5/babel.min.js"}) {
        WHEN("the client requests " << path << " without Brotli support") {
            const auto input = std::string("GET /") + path + " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip, deflate\r\n\r\n";
            Request    request;
            request.parseSomeData(input.begin(), input.end());
            auto response = handler.handleRequest(request);
            THEN("the dependency is served using gzip") {
                INFO(path);
                REQUIRE(response.statusCode == Response::ok);
                REQUIRE(response.getHeaderValue("Content-Encoding") == "gzip");
            }
        }
    }
}
