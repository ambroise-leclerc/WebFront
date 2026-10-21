#pragma once

#include <catch2/catch_test_macros.hpp>
#include <system/FileSystem.hpp>

#include <array>
#include <string>

// Read across word boundaries and beyond EOF: dataSize must exclude generated word padding.
inline std::string readEmbeddedAsset(webfront::fs::File& file, std::size_t size, bool gzip = true) {
    REQUIRE(file.isEncoded() == gzip);
    REQUIRE(file.getEncoding() == (gzip ? "gzip" : ""));
    std::string           content;
    std::array<char, 509> buffer{};
    while (auto count = file.read(buffer))
        content.append(buffer.data(), count);
    REQUIRE(content.size() == size);
    REQUIRE(file.eof());
    REQUIRE(file.read(buffer) == 0);
    if (gzip) {
        REQUIRE(content.size() > 18);
        REQUIRE(static_cast<unsigned char>(content[0]) == 0x1f);
        REQUIRE(static_cast<unsigned char>(content[1]) == 0x8b);
        REQUIRE(content[3] == 0);
        REQUIRE(content.substr(4, 4) == std::string(4, '\0'));
    }
    return content;
}
