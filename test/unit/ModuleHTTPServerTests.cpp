#include "Mocks.hpp"
#include "Bdd.hpp"
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace webfront::bdd;

using namespace webfront;
using namespace webfront::http;
using Net = networking::NetworkingMock;

namespace {
struct ModuleFile {
    static inline std::list names{"modules/main.mjs"};
};
/// A gzip-encoded module (1040 bytes decoded) served the way an embedded asset is.
struct GzipModuleData {
    static constexpr std::string_view encoding{"gzip"};
    static constexpr std::size_t      dataSize{55};
    static constexpr std::array<std::uint64_t, 7> data{
        0x1f8b080000000000, 0x02ff4bad28c82f2a, 0x5148cecf2b2e5148, 0xcc2b2e4f2d52b055,
        0x3031b2e64a1d9519, 0x95199519613200fb, 0x95b0441004000000};
};

struct GzipModuleFS {
    explicit GzipModuleFS(std::filesystem::path) { }

    static std::optional<fs::File> open(std::filesystem::path file) {
        if (file.relative_path().generic_string() == "modules/answer.mjs")
            return fs::File{GzipModuleData{}};
        return {};
    }
};

std::string decodedModule() {
    std::string text;
    for (int line = 0; line < 40; ++line)
        text += "export const answer = 42;\n";
    return text;
}
}  // namespace

Scenario requestHandlerServes{"RequestHandler serves JavaScript modules", [] {
    std::string input{"GET /modules/main.mjs HTTP/1.1\r\nHost: localhost\r\nAccept-encoding: gzip, br, deflate\r\n\r\n"};
    Request     request;
    requireNothrow([&] { request.parseSomeData(input.cbegin(), input.cend()); });

    RequestHandler<Net, MockFileSystem<ModuleFile>> handler{"."};
    auto                                            response = handler.handleRequest(request);

    require(response.statusCode == Response::ok);
    require(response.getHeaderValue("Content-Encoding") == "br");
    require(response.getHeaderValue("Content-Type") == "application/javascript");
    require(response.getHeaderValue("Content-Length") == "0");
}};

// WebKit does not advertise Brotli over loopback HTTP.

Scenario embeddedGzip{"Embedded gzip assets are served to gzip-only HTTP clients", [] {
    RequestHandler<Net, GzipModuleFS> handler{"."};
    when("the client requests the module without Brotli support", [&] {
        const std::string input = "GET /modules/answer.mjs HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip, deflate\r\n\r\n";
        Request           request;
        request.parseSomeData(input.begin(), input.end());
        auto response = handler.handleRequest(request);
        then("the stored gzip bytes are returned", [&] {
            require(response.statusCode == Response::ok);
            require(response.getHeaderValue("Content-Encoding") == "gzip");
            require(response.getHeaderValue("Vary") == "Accept-Encoding");
            require(response.getHeaderValue("Content-Length") == std::to_string(response.content.size()));
            require(response.content.size() == GzipModuleData::dataSize);
            require(response.getHeaderValue("Content-Type") == "application/javascript");
        });
    });
}};

Scenario embeddedGzipAssets{"Embedded gzip assets are served to identity-only HTTP clients", [] {
    RequestHandler<Net, GzipModuleFS> handler{"."};
    const auto                        expected = decodedModule();
    for (const auto accept : {"", "Accept-Encoding: identity\r\n", "Accept-Encoding: gzip;q=0, identity;q=1\r\n"}) {
        when("the client requests identity with ", accept, [&] {
            const auto input = std::string("GET /modules/answer.mjs HTTP/1.1\r\nHost: localhost\r\n") + accept + "\r\n";
            Request    request;
            request.parseSomeData(input.begin(), input.end());
            auto response = handler.handleRequest(request);
            then("the server returns the decoded bytes with their decoded length", [&] {
                require(response.statusCode == Response::ok);
                require(!response.getHeaderValue("Content-Encoding"));
                require(response.getHeaderValue("Content-Length") == std::to_string(expected.size()));
                require(response.content == expected);
                require(response.getHeaderValue("Content-Type") == "application/javascript");
                require(response.getHeaderValue("Vary") == "Accept-Encoding");
            });
        });
    }
}};
