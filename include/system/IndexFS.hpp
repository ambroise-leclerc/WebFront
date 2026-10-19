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
        static constexpr std::string_view         encoding{"gzip"};
        static constexpr size_t                   dataSize{510};
        static constexpr std::array<uint64_t, 64> data{
            0x1f8b080000000000, 0x02ff8d524f68d360, 0x14ffb56adc4171af, 0xa211c25cea1f500a, 0x0a8d94b14b863a19, 0x1e4485509c07831d, 0xdf3cac8408358740,
            0xe9657af1b0ab1451, 0x705e7670ae225a03, 0xb57af2a89789b7f5, 0x301ddbc18e40103c, 0xc497a4ceced37e5f, 0x7e79eff7beefbdef, 0x7df902a478a8ea20,
            0xbf77632d0d1c0670, 0x8aa932c79851fc7f, 0x0cf6ecc8b93c56ea, 0x19ac7f7b8999e9db, 0x78f1a482ef8df3b8, 0x7e65029df9d378ec, 0x9ec5fb774dcc562d,
            0xfcfef5136f1fa8b8, 0x5f1ec1dac7324a37, 0x2ec13bde5f35ef35, 0xb76f5357b7ebf4d3, 0xd8941f7e5a8e9d5d, 0x2660e20bd9360d6d, 0x2d9a911d86fdb745,
            0x899c04dd446ff6a4, 0x339c68d9b1195146, 0x2c5332c93d4415cc, 0x4db229864dc3bc17, 0x3ed03f7071b0c9c8, 0xadaaf2831d9edecf, 0x66a3ea919265e733,
            0xa4481f7373d73291, 0xae99d84774e8f5c6, 0xddacd23849d48674, 0x87d3b38da34bca99, 0xd5685e1a207257df, 0x504be9f0fab62995, 0xb91ce7e6e2facba6,
            0x6411b5dc0eb9373d, 0x6f89baa624fada21, 0xc1fa6bd23b7f1267, 0x489807c4341f8c87, 0xc3c7bc274c214ace, 0x160aa22b85c16cb1, 0xe8448f3369594204,
            0xe168d1302a15c330, 0x16c3a02b425d7fce, 0xbe513426fd422108, 0x425fd3c6a3c0baa6, 0x69256bd167adcd8f, 0x5fbc3caa697a30f5, 0xcaf2b5670b3138e0,
            0xe71b170abad6831e, 0x2ee4bce6a33ed4bd, 0x36df612dfac5768a, 0x345fca41e61ef64f, 0x1c0106e68009e6d8, 0x5560ee566d27256a, 0x09532bc0de3f23cd,
            0x980cfe0200000000,
        };
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
