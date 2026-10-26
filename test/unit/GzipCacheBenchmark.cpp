// Optional manual benchmark: cmake --build <build> --target gzip_cache_benchmark
#include "GzipFixtures.hpp"
#include <webfront/http/HTTPServer.hpp>
#include <webfront/networking/NetworkingMock.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct BundleFS {
    explicit BundleFS(std::filesystem::path) {}
    static inline std::vector<std::uint64_t> words;
    static inline std::size_t                size{};
    static void                              load(std::string_view bytes) {
        size = bytes.size();
        words.assign((size + 7) / 8, 0);
        for (std::size_t index = 0; index < size; ++index)
            words[index / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index])) << ((7 - index % 8) * 8);
    }
    std::optional<webfront::fs::File> open(std::filesystem::path) {
        return webfront::fs::File(words, size, "gzip");
    }
};

template <std::size_t CacheBudget>
void measure(std::string_view label, bool head, bool cold) {
    using namespace webfront::http;
    using Policy                  = CustomBuffersPolicy<8192, 8192, 32768, defaultMaxMessageSize, defaultMaxDecodedAssetSize, CacheBudget>;
    using Handler                 = RequestHandler<webfront::networking::NetworkingMock, BundleFS, Policy>;
    constexpr unsigned iterations = 200;
    Handler            handler(".");
    Request            request;
    request.method      = head ? Request::Method::Head : Request::Method::Get;
    request.uri         = "/bundle.js";
    std::size_t decodes = 0, retained = 0, length = 0;
    const auto  start = std::chrono::steady_clock::now();
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        auto run = [&](Handler& active) {
            const auto response = active.handleRequest(request);
            if (response.statusCode != Response::ok)
                throw std::runtime_error("fallback failed");
            length           = std::stoull(*response.getHeaderValue("Content-Length"));
            const auto stats = active.fallbackCacheStatistics();
            retained         = stats.retainedBytes;
            if (cold)
                decodes += stats.decodes;
            else
                decodes = stats.decodes;
        };
        if (cold) {
            Handler fresh(".");
            run(fresh);
        } else {
            run(handler);
        }
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    std::cout << label << ',' << (head ? "HEAD" : "GET") << ',' << (cold ? "cold" : "repeated") << ',' << BundleFS::size << ',' << length << ',' << iterations
              << ',' << decodes << ',' << retained << ',' << elapsed / iterations << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::string bytes;
        if (argc > 1) {
            std::ifstream file(argv[1], std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot open gzip bundle");
            bytes.assign(std::istreambuf_iterator<char>(file), {});
        }
        std::cout << "asset,method,requests,compressed_bytes,decoded_bytes,iterations,decodes,retained_bytes,us_per_request\n";
        for (const auto& [label, fixture] : {
                 std::pair{      "fixed",     gzipfixtures::fixed},
                 {    "dynamic",   gzipfixtures::dynamic},
                 {   "optional",  gzipfixtures::optional},
                 {"application", std::string_view(bytes)}
        }) {
            if (fixture.empty())
                continue;
            BundleFS::load(fixture);
            for (const bool head : {false, true}) {
                for (const bool cold : {false, true}) {
                    measure<0>(std::string(label) + "-disabled", head, cold);
                    measure<webfront::http::defaultMaxDecodedAssetCacheSize>(std::string(label) + "-cached", head, cold);
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
