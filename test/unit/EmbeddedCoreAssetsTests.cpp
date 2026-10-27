#include <catch2/catch_test_macros.hpp>
#include <webfront/system/DeveloperBootstrapData.hpp>
#include <webfront/system/DeveloperIndexHtmlData.hpp>
#include <webfront/system/FallbackIndexHtmlData.hpp>
#include <webfront/system/FallbackModuleData.hpp>
#include <webfront/system/FaviconData.hpp>
#include <webfront/system/WebFrontJsData.hpp>
#include <webfront/system/DomInterpreterData.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace webfront;

namespace {

/// The bytes a generated header embeds: its words are big-endian, the last one padded with zeros.
template <typename Data>
std::string embeddedBytes() {
    std::string bytes;
    for (auto word : Data::data)
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<char>((word >> shift) & 0xff));
    return bytes;
}

std::string sourceBytes(std::string_view relative) {
    std::ifstream file(std::filesystem::path(WEBFRONT_SOURCE_DIR) / relative, std::ios::binary);
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

template <typename Data>
void requireHeaderMatchesSource(std::string_view source) {
    INFO(source);
    REQUIRE(Data::encoding.empty());
    const auto expected = sourceBytes(source);
    const auto embedded = embeddedBytes<Data>();
    REQUIRE(Data::dataSize == expected.size());
    REQUIRE(embedded.size() >= expected.size());
    REQUIRE(embedded.size() - expected.size() < 8);
    REQUIRE(std::string_view(embedded).substr(0, expected.size()) == expected);
    REQUIRE(std::ranges::all_of(std::string_view(embedded).substr(expected.size()), [](char c) { return c == '\0'; }));
}

}  // namespace

SCENARIO("The embedded assets of the core are the readable sources they are generated from") {
    GIVEN("The generated headers of include/webfront/system and the sources of js/core") {
        THEN("Each header embeds exactly the bytes of its source, without compression and with zero padding only") {
            requireHeaderMatchesSource<fs::generated::DomInterpreterData>("js/core/dom-interpreter.mjs");
            requireHeaderMatchesSource<fs::generated::WebFrontJsData>("js/core/WebFront.js");
            requireHeaderMatchesSource<fs::generated::FallbackIndexHtmlData>("js/core/fallback/index.html");
            requireHeaderMatchesSource<fs::generated::FallbackModuleData>("js/core/fallback/webfront-fallback.mjs");
            requireHeaderMatchesSource<fs::generated::FaviconData>("js/core/favicon.ico");
            requireHeaderMatchesSource<fs::generated::DeveloperIndexHtmlData>("js/core/bootstrap/index.html");
            requireHeaderMatchesSource<fs::generated::DeveloperBootstrapData>("js/core/bootstrap/bootstrap.bundle.mjs");
        }
    }
}
