#include <system/IndexFS.hpp>
#include <tooling/HexDump.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

using namespace std;
using namespace webfront;

SCENARIO("IndexFileSystem provides basic files for browser support") {
    GIVEN("An IndexFS") {
        using FS = fs::IndexFS;
        WHEN("Requesting index.html") {
            auto indexFile = FS::open("index.html");
            THEN("A correct HTML5 file loading WebFront.js and the fallback module is returned") {
                REQUIRE(indexFile.has_value());

                // The readable source (src/fallback/index.html) is embedded raw, like WebFront.js,
                // so it is never Brotli-encoded; read it in full rather than assuming it fits a
                // single fixed-size buffer.
                REQUIRE_FALSE(indexFile->isEncoded());
                string html;
                array<char, 512> buffer;
                while (auto readSize = indexFile->read(buffer)) html.append(buffer.data(), readSize);

                REQUIRE(html.starts_with("<!DOCTYPE html>"));
                REQUIRE(regex_search(html, regex("<script[^>]*src=\"WebFront.js\"[^>]*>")));
                REQUIRE(regex_search(html, regex("<script[^>]*src=\"webfront-fallback.mjs\"[^>]*>")));
            }
        }

        WHEN("Requesting webfront-fallback.mjs") {
            auto moduleFile = FS::open("webfront-fallback.mjs");
            THEN("The fallback module awaiting webFront.ready is returned") {
                REQUIRE(moduleFile.has_value());
                REQUIRE_FALSE(moduleFile->isEncoded());
                REQUIRE(moduleFile->getEncoding().empty());

                string source;
                array<char, 512> buffer;
                while (auto readSize = moduleFile->read(buffer)) source.append(buffer.data(), readSize);
                REQUIRE(source.find("webFront.ready") != string::npos);
            }
        }

        WHEN("Requesting favicon.ico") {
            auto faviconFile = FS::open("favicon.ico");
            THEN("correct data is returned") {
                REQUIRE(faviconFile.has_value());
                array<uint8_t, 1024> buffer;
                auto readSize = faviconFile->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                array<uint8_t, 16> head, tail;
                if (faviconFile->isEncoded()) {
                    REQUIRE(faviconFile->getEncoding() == "br");
                    REQUIRE(readSize == 528);
                    head = {0xa1, 0xe8, 0x17, 0x00, 0xf7, 0x65, 0x80, 0x93, 0x2b, 0x7d, 0x81, 0x45, 0xc6, 0xc6, 0x10, 0x15};
                    tail = {0x30, 0xc1, 0xa7, 0xe5, 0x1d, 0x0d, 0x0c, 0x67, 0x76, 0x40, 0xa3, 0xc3, 0x84, 0xef, 0x01, 0x22};
                }
                else {
                    REQUIRE(readSize == 766);
                    head = {0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x20, 0x10, 0x00, 0x01, 0x00, 0x04, 0x00, 0xE8, 0x02};
                    tail = {0X00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x07};
                }
                REQUIRE(equal(head.begin(), head.end(), buffer.begin()));
                REQUIRE(equal(tail.begin(), tail.end(), buffer.begin() + readSize - tail.size()));
                readSize = faviconFile->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 0);
            }
        }
        WHEN("Requesting WebFront.js") {
            auto webfrontJSFile = FS::open("WebFront.js");
            THEN("Wbefront.js V0.0.1 content should be returned") {
                REQUIRE(webfrontJSFile.has_value());

                // WebFront.js is embedded verbatim rather than pre-compressed: deflate streams are not
                // reproducible across platforms, so the raw bytes are what the build can verify. Nothing
                // downstream advertises a Content-Encoding for it (see HTTPServer::handleRequest).
                REQUIRE_FALSE(webfrontJSFile->isEncoded());
                REQUIRE(webfrontJSFile->getEncoding().empty());

                array<char, 3> sourceHeader{};
                webfrontJSFile->read(sourceHeader);
                REQUIRE(sourceHeader == array<char, 3>{'/', '/', '/'});
            }
        }
    }
}
