#pragma once

#include "Bdd.hpp"
#include <webfront/system/FileSystem.hpp>

#include <array>
#include <string>

using namespace webfront::bdd;

// Read across word boundaries and beyond EOF: dataSize must exclude generated word padding.
inline std::string readEmbeddedAsset(webfront::fs::File& file, std::size_t size, bool gzip = true) {
    require(file.isEncoded() == gzip);
    require(file.getEncoding() == (gzip ? "gzip" : ""));
    std::string           content;
    std::array<char, 509> buffer{};
    while (auto count = file.read(buffer))
        content.append(buffer.data(), count);
    require(content.size() == size);
    require(file.eof());
    require(file.read(buffer) == 0);
    if (gzip) {
        require(content.size() > 18);
        require(static_cast<unsigned char>(content[0]) == 0x1f);
        require(static_cast<unsigned char>(content[1]) == 0x8b);
        require(content[3] == 0);
        require(content.substr(4, 4) == std::string(4, '\0'));
    }
    return content;
}
