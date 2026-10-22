#include "GzipFixtures.hpp"
#include <catch2/catch_test_macros.hpp>
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace webfront;
using namespace webfront::http;
using Net = networking::NetworkingMock;

namespace {
struct LegacyPolicy {
    static constexpr std::size_t receptionBufferSize    = 8192;
    static constexpr std::size_t emissionBufferSize     = 8192;
    static constexpr std::size_t functionCallBufferSize = 32768;
};
struct EncodedAssetFS {
    explicit EncodedAssetFS(std::filesystem::path) {}
    std::vector<std::uint64_t> words;

    std::optional<fs::File> open(std::filesystem::path path) {
        std::string bytes(gzipfixtures::fixed);
        if (path == "truncated.js")
            bytes.pop_back();
        if (path == "corrupt.js")
            bytes[bytes.size() - 8] ^= 1;
        if (path == "empty.js")
            bytes.clear();
        words.assign((bytes.size() + 7) / 8, 0);
        for (std::size_t index = 0; index < bytes.size(); ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index])) << ((7 - index % 8) * 8);
        if (path == "legacy.js")
            return fs::File(words, bytes.size(), "x-gzip");
        return fs::File(words, bytes.size(), path == "unknown.js" ? "br" : "gzip");
    }
};
struct AssetServerNet : Net {
    static inline unsigned    accepts = 0;
    static inline std::string statusLine;

    static auto Accept(Acceptor&) {
        return networking::ex::then(networking::ex::just(), []() -> Socket {
            if (accepts++ != 0)
                throw std::system_error(std::make_error_code(std::errc::operation_canceled));
            return Socket{};
        });
    }

    static auto Receive(Socket&, MutableBuffer buffer) {
        return networking::ex::then(networking::ex::just(), [buffer] {
            constexpr std::string_view request = "GET /asset.js HTTP/1.1\r\nHost: localhost\r\n\r\n";
            std::copy(request.begin(), request.end(), static_cast<char*>(buffer.data()));
            return request.size();
        });
    }

    static auto Send(Socket&, const auto& buffers) {
        const auto& first = buffers.front();
        statusLine.assign(static_cast<const char*>(first.data()), first.size());
        return networking::ex::just(std::size_t{0});
    }
};

Request assetRequest() {
    Request request;
    request.method = Request::Method::Get;
    request.uri    = "/asset.js";
    return request;
}
}  // namespace


SCENARIO("HTTP decodes gzip when the stored encoding is unavailable") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto header : {"", "br", "gzip;q=0", "gzip;q=0.000, *;q=1", "xgzip", "gzip2", "gzip;q=invalid"}) {
        GIVEN("Accept-Encoding: " << header) {
            auto request = assetRequest();
            request.headers.emplace_back("Accept-Encoding", header);
            const auto response = handler.handleRequest(request);
            THEN("the body contains the decoded asset") {
                REQUIRE(response.statusCode == Response::ok);
                REQUIRE(response.content.starts_with("WebFront gzip fallback\n"));
                REQUIRE(response.content.size() == 23768);
            }
            THEN("encoding metadata describes identity") {
                REQUIRE_FALSE(response.getHeaderValue("Content-Encoding"));
                REQUIRE(response.getHeaderValue("Content-Length") == "23768");
                REQUIRE(response.getHeaderValue("Vary") == "Accept-Encoding");
            }
            THEN("the resource keeps its JavaScript MIME type") {
                REQUIRE(response.getHeaderValue("Content-Type") == "application/javascript");
            }
        }
    }
}

SCENARIO("HTTP preserves accepted gzip bytes") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto header : {"gzip", "GZIP", "x-gzip", "X-GZIP;q=0.5", "br, gzip;q=0.5", "gzip;q=1.000", "*;q=0.1", "gzip;q=1, *;q=0"}) {
        GIVEN("Accept-Encoding: " << header) {
            auto request = assetRequest();
            request.headers.emplace_back("Accept-Encoding", header);
            const auto response = handler.handleRequest(request);
            THEN("the server returns the original encoded body") {
                REQUIRE(response.statusCode == Response::ok);
                REQUIRE(response.content == gzipfixtures::fixed);
            }
            THEN("metadata describes the gzip representation") {
                REQUIRE(response.getHeaderValue("Content-Length") == std::to_string(gzipfixtures::fixed.size()));
                REQUIRE(response.getHeaderValue("Content-Encoding") == "gzip");
                REQUIRE(response.getHeaderValue("Vary") == "Accept-Encoding");
            }
        }
    }
}

SCENARIO("HTTP considers explicit encoding preferences across fields") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    GIVEN("multiple Accept-Encoding fields") {
        request.headers.emplace_back("Accept-Encoding", "br");
        request.headers.emplace_back("accept-encoding", "gzip;q=0.5");
        THEN("all fields participate in negotiation") {
            REQUIRE(handler.handleRequest(request).getHeaderValue("Content-Encoding") == "gzip");
        }
    }
    GIVEN("an explicit identity preference overrides the wildcard exclusion") {
        request.headers.emplace_back("Accept-Encoding", "gzip;q=0, *;q=0, identity;q=0.5");
        THEN("the server can serve identity") {
            REQUIRE(handler.handleRequest(request).statusCode == Response::ok);
        }
    }
}

SCENARIO("HTTP rejects unavailable representations") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    for (const auto header : {"gzip;q=0, identity;q=0", "*;q=0"}) {
        GIVEN("Accept-Encoding: " << header) {
            request.headers.emplace_back("Accept-Encoding", header);
            THEN("the server responds with 406") {
                REQUIRE(handler.handleRequest(request).statusCode == Response::notAcceptable);
            }
        }
    }
    GIVEN("an unavailable custom content coding") {
        request.uri = "/unknown.js";
        THEN("it is rejected unless the client accepts it directly") {
            REQUIRE(handler.handleRequest(request).statusCode == Response::notAcceptable);
            request.headers.emplace_back("Accept-Encoding", "br");
            REQUIRE(handler.handleRequest(request).getHeaderValue("Content-Encoding") == "br");
        }
    }
}

SCENARIO("HTTP HEAD describes the negotiated representation") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto& [header, length] : {
             std::pair{    "",                       std::string("23768")},
             {"gzip", std::to_string(gzipfixtures::fixed.size())}
    }) {
        GIVEN("Accept-Encoding: " << header) {
            auto request   = assetRequest();
            request.method = Request::Method::Head;
            request.headers.emplace_back("Accept-Encoding", header);
            const auto response = handler.handleRequest(request);
            THEN("no body is sent") {
                REQUIRE(response.statusCode == Response::ok);
                REQUIRE(response.content.empty());
            }
            THEN("headers match the corresponding GET representation") {
                REQUIRE(response.getHeaderValue("Content-Length") == length);
                REQUIRE(response.getHeaderValue("Content-Encoding").value_or("") == header);
                REQUIRE(response.getHeaderValue("Vary") == "Accept-Encoding");
            }
        }
    }
}

SCENARIO("HTTP fallback rejects corrupt and truncated streams") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto path : {"/corrupt.js", "/empty.js", "/truncated.js"}) {
        GIVEN(path) {
            auto request        = assetRequest();
            request.uri         = path;
            const auto response = handler.handleRequest(request);
            THEN("a complete error response is returned") {
                REQUIRE(response.statusCode == Response::internalServerError);
                REQUIRE(response.getHeaderValue("Content-Length") == std::to_string(response.content.size()));
            }
            THEN("no partial decoded output is exposed") {
                REQUIRE_FALSE(response.getHeaderValue("Content-Encoding"));
                REQUIRE(response.content.find("WebFront gzip fallback") == std::string::npos);
            }
        }
    }
}

SCENARIO("HTTP gzip fallback respects the configured decoded byte budget") {
    using Limited = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, 23767>;
    RequestHandler<Net, EncodedAssetFS, Limited> handler{"."};
    auto                                         request = assetRequest();
    GIVEN("GET and HEAD identity responses exceeding the budget by one byte") {
        for (const auto method : {Request::Method::Get, Request::Method::Head}) {
            request.method = method;
            THEN("the server rejects the oversized decoded representation for method " << static_cast<int>(method)) {
                REQUIRE(handler.handleRequest(request).statusCode == Response::internalServerError);
            }
        }
    }
    GIVEN("a client accepting the stored gzip representation") {
        request.headers.emplace_back("Accept-Encoding", "gzip");
        THEN("pass-through is independent of the decoded byte budget") {
            REQUIRE(handler.handleRequest(request).statusCode == Response::ok);
        }
    }
    GIVEN("a budget exactly matching the decoded size") {
        using Exact = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, 23768>;
        RequestHandler<Net, EncodedAssetFS, Exact> exact{"."};
        THEN("the identity response is allowed") {
            REQUIRE(exact.handleRequest(request).content.size() == 23768);
        }
    }
}

SCENARIO("Gzip fallback budgets preserve legacy buffer policy compatibility") {
    THEN("an older policy inherits the 4 MiB fallback limit") {
        REQUIRE(maxDecodedAssetSize<LegacyPolicy>() == 4 * 1024 * 1024);
        REQUIRE(maxDecodedAssetSize<DefaultBuffersPolicy>() == defaultMaxDecodedAssetSize);
    }
}

SCENARIO("HTTP Server forwards the application policy to gzip fallback") {
    AssetServerNet::accepts = 0;
    AssetServerNet::statusLine.clear();
    using Limited = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, 23767>;
    GIVEN("an application policy below the requested asset size") {
        Server<AssetServerNet, EncodedAssetFS, Limited> server("127.0.0.1", "0");
        THEN("the connection receives the bounded fallback error") {
            REQUIRE(AssetServerNet::statusLine == "HTTP/1.1 500 Internal Server Error\r\n");
        }
    }
}

SCENARIO("HTTP treats stored x-gzip assets as gzip for fallback and pass-through") {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    request.uri                                 = "/legacy.js";
    GIVEN("a client needing identity") {
        const auto response = handler.handleRequest(request);
        THEN("the alias is decoded") {
            REQUIRE(response.statusCode == Response::ok);
            REQUIRE_FALSE(response.getHeaderValue("Content-Encoding"));
            REQUIRE(response.content.starts_with("WebFront gzip fallback\n"));
        }
    }
    GIVEN("a client accepting gzip") {
        request.headers.emplace_back("Accept-Encoding", "gzip");
        const auto response = handler.handleRequest(request);
        THEN("the original stored bytes and coding label are retained") {
            REQUIRE(response.statusCode == Response::ok);
            REQUIRE(response.getHeaderValue("Content-Encoding") == "x-gzip");
            REQUIRE(response.content == gzipfixtures::fixed);
        }
    }
}
