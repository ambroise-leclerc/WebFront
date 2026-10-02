/// @date 26/04/2023 22:39:42
/// @author Ambroise Leclerc
/// @brief Virtual file system providing access to WebFront.js, favicon.ico and a minimal index.html.
#pragma once

#include "FallbackIndexHtmlData.hpp"
#include "FallbackModuleData.hpp"
#include "FileSystem.hpp"
#include "WebFrontJsData.hpp"

#include <filesystem>

namespace webfront::fs {

class IndexFS {
public:
    IndexFS(std::filesystem::path /*docRoot*/) {}
    IndexFS() = delete;
    IndexFS(const IndexFS&) = default;
    IndexFS(IndexFS&&) = default;
    IndexFS& operator=(const IndexFS&) = default;
    IndexFS& operator=(IndexFS&&) = default;

    using IndexHtml = generated::FallbackIndexHtmlData;
    using FallbackModule = generated::FallbackModuleData;

    struct WebFrontIco {
        static constexpr std::string_view encoding{"br"};
        static constexpr size_t dataSize{528};
        static constexpr std::array<uint64_t, 67> data{
          0xa1e81700f7658093, 0x2b7d8145c6c61015, 0xc4c80e31aa99183d, 0x54378e5eed6ed133, 0xfaca20ffde26c4fe,
          0x907c01bcd106aa84, 0x16c239717525dc81, 0x22b0dd830e70c053, 0x2d28ed2ecb3c4fbc, 0x0430c0d5c91daf4f,
          0x2cd10c1eb53020ac, 0xffcdde67d7cf55af, 0xdca9e2830d38c108, 0x02e090573dedc964, 0x4a677eea547dcc18,
          0x9b828b2818b06030, 0xb1d56e2bc8a235a8, 0x57f5b1c17b35af71, 0x09dcb5b387b49237, 0x2bdf037ce083b6b6,
          0x2cf8208817fca008, 0xc0188036006fe0f1, 0xe8c664f10f9ded6d, 0xf1602cf0eafe452b, 0xccc1e6541d9eed16,
          0x622383f1703e31d1, 0x6c85e32b9771da2a, 0xf1fbbb4b1b9de5ce, 0x7871d6e52607625f, 0x0f11b7ddbf8ce898,
          0x36fafcd39cf2feab, 0xdf7b812c200bd7c9, 0xa04403e83225c651, 0x5970c2e4d00fc03f, 0x8a6a815662143084,
          0x42e7539252581640, 0xd64742491a255ac8, 0x024e89050883ccf0, 0x948edad49eb3b200, 0x2932fd7a3fa2a643,
          0x0caee9e2e8369bc5, 0xf0a48e2c487a14ce, 0xbf9ea9b66b209da8, 0x688fa7b3abb9a366, 0xf9b40909d3d7fc74,
          0x3a527b181930c5eb, 0xea13b3cf8b3ba3b3, 0x6a80e6e995babfbf, 0x433e003e386581fe, 0x634428651a6664dc,
          0x8cd11a312546eb0d, 0x0b0a7259de712cf8, 0x204c13d9fc84a554, 0x5616147cfb2fc7d5, 0xd5711cb7f5dfc302,
          0xe7762297f2d9d1f1, 0x1bb50b483ff7cab2, 0xcbaddcfaecb29b0f, 0xf00f75b5bcbebdca, 0xfedcaabc563fb7eb,
          0xe7e86527d9eb7fec, 0x5f1ef92563fb2708, 0x74c08718f1cb0272, 0xcb0d2100faca96e8, 0x30c1a7e51d0d0c67,
          0x7640a3c384ef0122};
    };

    using WebFrontJs = generated::WebFrontJsData;

    static std::optional<File> open(std::filesystem::path file) {
        auto filename = file.relative_path().string();
        if (filename == "index.html") return File(IndexHtml{});
        if (filename == "favicon.ico") return File{WebFrontIco{}};
        if (filename == "WebFront.js") return File{WebFrontJs{}};
        if (filename == "webfront-fallback.mjs") return File{FallbackModule{}};
        return {};
    }
};

} // namespace webfront::fs
