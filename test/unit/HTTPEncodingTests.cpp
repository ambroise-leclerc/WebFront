#include "GzipFixtures.hpp"
#include "Bdd.hpp"
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace webfront::bdd;

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
    static inline unsigned                accepts = 0;
    static inline std::string             statusLine;
    static inline std::mutex              mutex;
    static inline std::condition_variable sent;
    static std::string                    waitForResponse() {
        std::unique_lock lock(mutex);
        sent.wait(lock, [] {
            return !statusLine.empty();
        });
        return statusLine;
    }

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
        {
            std::lock_guard lock(mutex);
            statusLine.assign(static_cast<const char*>(first.data()), first.size());
        }
        sent.notify_all();
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


const Scenario hTTPDecodes{"HTTP decodes gzip when the stored encoding is unavailable", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto header : {"", "br", "gzip;q=0", "gzip;q=0.000, *;q=1", "xgzip", "gzip2", "gzip;q=invalid"}) {
        given("Accept-Encoding: ", header, [&] {
            auto request = assetRequest();
            request.headers.emplace_back("Accept-Encoding", header);
            const auto response = handler.handleRequest(request);
            then("the body contains the decoded asset", [&] {
                require(response.statusCode == Response::ok);
                require(response.content.starts_with("WebFront gzip fallback\n"));
                require(response.content.size() == 23768);
            });
            then("encoding metadata describes identity", [&] {
                require(!response.getHeaderValue("Content-Encoding"));
                require(response.getHeaderValue("Content-Length") == "23768");
                require(response.getHeaderValue("Vary") == "Accept-Encoding");
            });
            then("the resource keeps its JavaScript MIME type", [&] {
                require(response.getHeaderValue("Content-Type") == "application/javascript");
            });
        });
    }
}};

const Scenario hTTPPreserves{"HTTP preserves accepted gzip bytes", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto header : {"gzip", "GZIP", "x-gzip", "X-GZIP;q=0.5", "br, gzip;q=0.5", "gzip;q=1.000", "*;q=0.1", "gzip;q=1, *;q=0"}) {
        given("Accept-Encoding: ", header, [&] {
            auto request = assetRequest();
            request.headers.emplace_back("Accept-Encoding", header);
            const auto response = handler.handleRequest(request);
            then("the server returns the original encoded body", [&] {
                require(response.statusCode == Response::ok);
                require(response.content == gzipfixtures::fixed);
            });
            then("metadata describes the gzip representation", [&] {
                require(response.getHeaderValue("Content-Length") == std::to_string(gzipfixtures::fixed.size()));
                require(response.getHeaderValue("Content-Encoding") == "gzip");
                require(response.getHeaderValue("Vary") == "Accept-Encoding");
            });
        });
    }
}};

const Scenario hTTPConsiders{"HTTP considers explicit encoding preferences across fields", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    given("multiple Accept-Encoding fields", [&] {
        request.headers.emplace_back("Accept-Encoding", "br");
        request.headers.emplace_back("accept-encoding", "gzip;q=0.5");
        then("all fields participate in negotiation", [&] {
            require(handler.handleRequest(request).getHeaderValue("Content-Encoding") == "gzip");
        });
    });
    given("an explicit identity preference overrides the wildcard exclusion", [&] {
        request.headers.emplace_back("Accept-Encoding", "gzip;q=0, *;q=0, identity;q=0.5");
        then("the server can serve identity", [&] {
            require(handler.handleRequest(request).statusCode == Response::ok);
        });
    });
}};

const Scenario hTTPRejectsUnavailable{"HTTP rejects unavailable representations", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    for (const auto header : {"gzip;q=0, identity;q=0", "*;q=0"}) {
        given("Accept-Encoding: ", header, [&] {
            request.headers.emplace_back("Accept-Encoding", header);
            then("the server responds with 406", [&] {
                require(handler.handleRequest(request).statusCode == Response::notAcceptable);
            });
        });
    }
    given("an unavailable custom content coding", [&] {
        request.uri = "/unknown.js";
        then("it is rejected unless the client accepts it directly", [&] {
            require(handler.handleRequest(request).statusCode == Response::notAcceptable);
            request.headers.emplace_back("Accept-Encoding", "br");
            require(handler.handleRequest(request).getHeaderValue("Content-Encoding") == "br");
        });
    });
}};

const Scenario hTTPHEAD{"HTTP HEAD describes the negotiated representation", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto& [header, length] : {
             std::pair{    "",                       std::string("23768")},
             {"gzip", std::to_string(gzipfixtures::fixed.size())}
    }) {
        given("Accept-Encoding: ", header, [&] {
            auto request   = assetRequest();
            request.method = Request::Method::Head;
            request.headers.emplace_back("Accept-Encoding", header);
            const auto response = handler.handleRequest(request);
            then("no body is sent", [&] {
                require(response.statusCode == Response::ok);
                require(response.content.empty());
            });
            then("headers match the corresponding GET representation", [&] {
                require(response.getHeaderValue("Content-Length") == length);
                require(response.getHeaderValue("Content-Encoding").value_or("") == header);
                require(response.getHeaderValue("Vary") == "Accept-Encoding");
            });
        });
    }
}};

const Scenario hTTPFallback{"HTTP fallback rejects corrupt and truncated streams", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    for (const auto path : {"/corrupt.js", "/empty.js", "/truncated.js"}) {
        given(path, [&] {
            auto request        = assetRequest();
            request.uri         = path;
            const auto response = handler.handleRequest(request);
            then("a complete error response is returned", [&] {
                require(response.statusCode == Response::internalServerError);
                require(response.getHeaderValue("Content-Length") == std::to_string(response.content.size()));
            });
            then("no partial decoded output is exposed", [&] {
                require(!response.getHeaderValue("Content-Encoding"));
                require(response.content.find("WebFront gzip fallback") == std::string::npos);
            });
        });
    }
}};

const Scenario hTTPGzipFallback{"HTTP gzip fallback respects the configured decoded byte budget", [] {
    using Limited = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, 23767>;
    RequestHandler<Net, EncodedAssetFS, Limited> handler{"."};
    auto                                         request = assetRequest();
    given("GET and HEAD identity responses exceeding the budget by one byte", [&] {
        for (const auto method : {Request::Method::Get, Request::Method::Head}) {
            request.method = method;
            then("the server rejects the oversized decoded representation for method ", static_cast<int>(method), [&] {
                require(handler.handleRequest(request).statusCode == Response::internalServerError);
            });
        }
    });
    given("a client accepting the stored gzip representation", [&] {
        request.headers.emplace_back("Accept-Encoding", "gzip");
        then("pass-through is independent of the decoded byte budget", [&] {
            require(handler.handleRequest(request).statusCode == Response::ok);
        });
    });
    given("a budget exactly matching the decoded size", [&] {
        using Exact = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, 23768>;
        RequestHandler<Net, EncodedAssetFS, Exact> exact{"."};
        then("the identity response is allowed", [&] {
            require(exact.handleRequest(request).content.size() == 23768);
        });
    });
}};

const Scenario gzipFallback{"Gzip fallback budgets preserve legacy buffer policy compatibility", [] {
    then("an older policy inherits the 4 MiB fallback limit", [&] {
        require(maxDecodedAssetSize<LegacyPolicy>() == 4 * 1024 * 1024);
        require(maxDecodedAssetSize<DefaultBuffersPolicy>() == defaultMaxDecodedAssetSize);
    });
}};

const Scenario hTTPServer{"HTTP Server forwards the application policy to gzip fallback", [] {
    AssetServerNet::accepts = 0;
    AssetServerNet::statusLine.clear();
    using Limited = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, 23767>;
    given("an application policy below the requested asset size", [&] {
        Server<AssetServerNet, EncodedAssetFS, Limited> server("127.0.0.1", "0");
        then("the connection receives the bounded fallback error", [&] {
            require(AssetServerNet::waitForResponse() == "HTTP/1.1 500 Internal Server Error\r\n");
        });
    });
}};

const Scenario hTTPTreats{"HTTP treats stored x-gzip assets as gzip for fallback and pass-through", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    request.uri                                 = "/legacy.js";
    given("a client needing identity", [&] {
        const auto response = handler.handleRequest(request);
        then("the alias is decoded", [&] {
            require(response.statusCode == Response::ok);
            require(!response.getHeaderValue("Content-Encoding"));
            require(response.content.starts_with("WebFront gzip fallback\n"));
        });
    });
    given("a client accepting gzip", [&] {
        request.headers.emplace_back("Accept-Encoding", "gzip");
        const auto response = handler.handleRequest(request);
        then("the original stored bytes and coding label are retained", [&] {
            require(response.statusCode == Response::ok);
            require(response.getHeaderValue("Content-Encoding") == "x-gzip");
            require(response.content == gzipfixtures::fixed);
        });
    });
}};

const Scenario hTTPGET{"HTTP GET and HEAD reuse validated gzip revisions across paths and aliases", [] {
    RequestHandler<Net, EncodedAssetFS> handler{"."};
    auto                                request = assetRequest();
    given("a cold HEAD followed by GET, another path and the x-gzip alias", [&] {
        request.method = Request::Method::Head;
        require(handler.handleRequest(request).content.empty());
        request.method = Request::Method::Get;
        require(handler.handleRequest(request).content.size() == 23768);
        request.uri = "/other.js";
        require(handler.handleRequest(request).content.size() == 23768);
        request.uri = "/legacy.js";
        require(handler.handleRequest(request).content.size() == 23768);
        then("identical compressed bytes require a single decode", [&] {
            require(handler.fallbackCacheStatistics().decodes == 1);
            require(handler.fallbackCacheStatistics().hits == 3);
        });
        then("gzip pass-through leaves the fallback cache untouched", [&] {
            request.headers.emplace_back("Accept-Encoding", "gzip");
            require(handler.handleRequest(request).content == gzipfixtures::fixed);
            require(handler.fallbackCacheStatistics().hits == 3);
        });
    });
}};

namespace {
struct MutableAssetFS : EncodedAssetFS {
    using EncodedAssetFS::EncodedAssetFS;
    static inline std::string revision;
    std::optional<fs::File>   open(std::filesystem::path) {
        words.assign((revision.size() + 7) / 8, 0);
        for (std::size_t index = 0; index < revision.size(); ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(revision[index])) << ((7 - index % 8) * 8);
        return fs::File(words, revision.size(), "gzip");
    }
};
}  // namespace

const Scenario hTTPMutable{"HTTP mutable providers cannot reuse stale identity content or HEAD lengths", [] {
    RequestHandler<Net, MutableAssetFS> handler{"."};
    auto                                request = assetRequest();
    MutableAssetFS::revision                    = gzipfixtures::fixed;
    require(handler.handleRequest(request).content.size() == 23768);
    given("the provider replaces the same path with two concatenated members", [&] {
        MutableAssetFS::revision += gzipfixtures::fixed;
        request.method            = Request::Method::Head;
        const auto head           = handler.handleRequest(request);
        require(head.getHeaderValue("Content-Length") == "47536");
        require(head.content.empty());
        request.method = Request::Method::Get;
        require(handler.handleRequest(request).content.size() == 47536);
        require(handler.fallbackCacheStatistics().decodes == 2);
        require(handler.fallbackCacheStatistics().hits == 1);
    });
    given("the provider replaces it with corrupt content having the same ISIZE", [&] {
        MutableAssetFS::revision[MutableAssetFS::revision.size() - 8] ^= 1;
        for (const auto method : {Request::Method::Get, Request::Method::Head}) {
            request.method = method;
            require(handler.handleRequest(request).statusCode == Response::internalServerError);
        }
        require(handler.fallbackCacheStatistics().decodes == 3);
        require(handler.fallbackCacheStatistics().entries == 1);
    });
}};

const Scenario hTTPCache{"HTTP cache ownership and policy remain local to the application", [] {
    using Disabled = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, defaultMaxDecodedAssetSize, 0>;
    RequestHandler<Net, EncodedAssetFS, Disabled> handler{"."};
    const auto                                    request = assetRequest();
    given("a policy disabling cache retention", [&] {
        require(handler.handleRequest(request).statusCode == Response::ok);
        require(handler.handleRequest(request).statusCode == Response::ok);
        require(handler.fallbackCacheStatistics().decodes == 2);
        require(handler.fallbackCacheStatistics().retainedBytes == 0);
    });
    then("legacy policies retain the default cache budget", [&] {
        require(maxDecodedAssetCacheSize<LegacyPolicy>() == defaultMaxDecodedAssetCacheSize);
    });
    given("two independent handlers", [&] {
        RequestHandler<Net, EncodedAssetFS> first{"."}, second{"."};
        require(first.handleRequest(request).statusCode == Response::ok);
        require(second.fallbackCacheStatistics().entries == 0);
        require(second.handleRequest(request).statusCode == Response::ok);
        require(second.fallbackCacheStatistics().decodes == 1);
    });
}};

const Scenario hTTPServerDrains{"HTTP server drains and destroys a populated fallback cache", [] {
    given("successive applications serving an identity fallback", [&] {
        for (unsigned instance = 0; instance < 2; ++instance) {
            AssetServerNet::accepts = 0;
            AssetServerNet::statusLine.clear();
            {
                Server<AssetServerNet, EncodedAssetFS> server("127.0.0.1", "0");
                require(AssetServerNet::waitForResponse() == "HTTP/1.1 200 OK\r\n");
                server.stop();
                server.drain();
            }
            then("shutdown completes with no cache work outstanding", [&] {
                require(AssetServerNet::accepts >= 1);
            });
        }
    });
}};

namespace {
struct ActiveAssetBudget : DefaultBuffersPolicy {
    static inline thread_local std::size_t maxDecodedAssetSize = defaultMaxDecodedAssetSize;

    explicit ActiveAssetBudget(std::size_t limit) : previous(std::exchange(maxDecodedAssetSize, limit)) {}
    ~ActiveAssetBudget() {
        maxDecodedAssetSize = previous;
    }

private:
    std::size_t previous;
};
}  // namespace

const Scenario hTTPCached{"HTTP cached GET and HEAD recheck the active application output budget", [] {
    const ActiveAssetBudget                                initialBudget{23768};
    RequestHandler<Net, EncodedAssetFS, ActiveAssetBudget> handler{"."};
    auto                                                   request = assetRequest();
    require(handler.handleRequest(request).statusCode == Response::ok);
    given("the application lowers its budget after validating this revision", [&] {
        {
            const ActiveAssetBudget smallerBudget{23767};
            for (const auto method : {Request::Method::Get, Request::Method::Head}) {
                request.method      = method;
                const auto response = handler.handleRequest(request);
                require(response.statusCode == Response::internalServerError);
                require(response.getHeaderValue("Vary") == "Accept-Encoding");
            }
            require(handler.fallbackCacheStatistics().decodes == 1);
            require(handler.fallbackCacheStatistics().hits == 2);
        }
        require(handler.handleRequest(request).getHeaderValue("Content-Length") == "23768");
    });
}};
