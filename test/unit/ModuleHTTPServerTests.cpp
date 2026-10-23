#include "Mocks.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>
#include <webfront/system/BabelFS.hpp>
#include <webfront/system/JasmineFS.hpp>
#include <webfront/system/ReactFS.hpp>

#include <array>
#include <fstream>
#include <iterator>
#include <list>
#include <string>
#include <string_view>
#include <utility>

using namespace webfront;
using namespace webfront::http;
using Net = networking::NetworkingMock;

namespace {
struct ModuleFile {
    static inline std::list names{"modules/main.mjs"};
};
constexpr std::array<std::pair<std::string_view, std::string_view>, 6> dependencies{
    {
     {"jasmine/7.0.2/jasmine.js", "jasmine/jasmine.js"},
     {"jasmine/7.0.2/jasmine-html.js", "jasmine/jasmine-html.js"},
     {"jasmine/7.0.2/jasmine.css", "jasmine/jasmine.css"},
     {"jasmine/7.0.2/boot.js", "jasmine/boot.js"},
     {"react@19.3.1/runtime.mjs", "react/runtime.mjs"},
     {"babel-standalone@8.0.7/babel.min.js", "babel/babel.min.js"},
     }
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

// WebKit does not advertise Brotli over loopback HTTP.

SCENARIO("Embedded browser dependencies support gzip-only HTTP clients") {
    using Filesystem = fs::Multi<fs::JasmineFS, fs::ReactFS, fs::BabelFS>;
    RequestHandler<Net, Filesystem> handler{"."};
    for (const auto& [path, sourceName] : dependencies) {
        WHEN("the client requests " << path << " without Brotli support") {
            const auto input = std::string("GET /") + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip, deflate\r\n\r\n";
            Request    request;
            request.parseSomeData(input.begin(), input.end());
            auto response = handler.handleRequest(request);
            THEN("the dependency is served using gzip") {
                INFO(path);
                REQUIRE(response.statusCode == Response::ok);
                REQUIRE(response.getHeaderValue("Content-Encoding") == "gzip");
                REQUIRE(response.getHeaderValue("Vary") == "Accept-Encoding");
                REQUIRE(response.getHeaderValue("Content-Length") == std::to_string(response.content.size()));
                auto file = Filesystem{"."}.open(path);
                REQUIRE(file.has_value());
                std::string           expected;
                std::array<char, 509> buffer{};
                while (auto count = file->read(buffer))
                    expected.append(buffer.data(), count);
                REQUIRE(response.content == expected);
                REQUIRE(response.getHeaderValue("Content-Type") == (std::string_view(path).ends_with(".css") ? "text/css" : "application/javascript"));
            }
        }
    }
}

SCENARIO("Embedded browser dependencies support identity-only HTTP clients") {
    using Filesystem = fs::Multi<fs::JasmineFS, fs::ReactFS, fs::BabelFS>;
    RequestHandler<Net, Filesystem> handler{"."};
    for (const auto& [path, sourceName] : dependencies) {
        GIVEN(path) {
            const auto    sourcePath = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "third_party" / sourceName;
            std::ifstream source{sourcePath, std::ios::binary};
            REQUIRE(source.is_open());
            const std::string expected{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
            REQUIRE_FALSE(expected.empty());
            for (const auto accept : {"", "Accept-Encoding: identity\r\n", "Accept-Encoding: gzip;q=0, identity;q=1\r\n"}) {
                WHEN("the client requests identity with " << accept) {
                    const auto input = std::string("GET /") + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\n" + accept + "\r\n";
                    Request    request;
                    request.parseSomeData(input.begin(), input.end());
                    auto response = handler.handleRequest(request);
                    THEN("the server returns the upstream bytes with their decoded length") {
                        REQUIRE(response.statusCode == Response::ok);
                        REQUIRE_FALSE(response.getHeaderValue("Content-Encoding"));
                        REQUIRE(response.getHeaderValue("Content-Length") == std::to_string(expected.size()));
                        REQUIRE(response.content == expected);
                        REQUIRE(response.getHeaderValue("Content-Type") == (path.ends_with(".css") ? "text/css" : "application/javascript"));
                        REQUIRE(response.getHeaderValue("Vary") == "Accept-Encoding");
                    }
                }
            }
        }
    }
}
