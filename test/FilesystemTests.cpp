#include <system/Filesystem.hpp>
#include <tooling/HexDump.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

using namespace std;
using namespace webfront;

SCENARIO("IndexFileSystem provides basic files for browser support") {
    GIVEN("An IndexFS") {
        using FS = webfront::filesystem::IndexFS;
        WHEN("Requesting index.html") {
            auto indexFile = FS::open("index.html");
            THEN("A correct HTML5 file with the WebFront.js script inclusion is returned") {
                REQUIRE(indexFile.has_value());
                array<char, 1024> buffer;
                auto readSize = indexFile->read(buffer);

                string html{buffer.data(), indexFile->gcount()};
                if (indexFile->isEncoded()) {
                }
                else {
                    REQUIRE(html.starts_with("<!DOCTYPE html>"));

                    smatch match;
                    REQUIRE(regex_search(html, match, regex("<script.*src=\"WebFront.js\".*>")));
                }
            }
        }
    
    WHEN("Requesting favicon.ico") {
        auto faviconFile = FS::open("favicon.ico");
        THEN("correct data is returned") {
            REQUIRE(faviconFile.has_value());
            REQUIRE(!faviconFile->isEncoded());

            array<uint8_t, 1024> buffer;
            auto readSize = faviconFile->read(reinterpret_cast<char*>(buffer.data()), buffer.size()).gcount();
            REQUIRE(readSize == 766);

            array<uint8_t, 16> head{0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x20,
                                    0x10, 0x00, 0x01, 0x00, 0x04, 0x00, 0xE8, 0x02};
            array<uint8_t, 16> tail{0X00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                    0x80, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x07};
            REQUIRE(equal(head.begin(), head.end(), buffer.begin()));
            REQUIRE(equal(tail.begin(), tail.end(), buffer.begin() + readSize - tail.size()));

            readSize = faviconFile->read(reinterpret_cast<char*>(buffer.data()), buffer.size()).gcount();
            REQUIRE(readSize == 0);
        }
    }
    WHEN("Requesting WebFront.js") {
        auto webfrontJSFile = FS::open("WebFront.js");
        THEN("Wbefront.js V0.0.1 content should be returned") {
            REQUIRE(webfrontJSFile.has_value());

            REQUIRE(webfrontJSFile->isEncoded());
            REQUIRE(webfrontJSFile->getEncoding() == "gzip");

            array<uint8_t, 10> gzipHeader;
            webfrontJSFile->read(reinterpret_cast<char*>(gzipHeader.data()), gzipHeader.size());

            REQUIRE(gzipHeader[0] == 0x1f);
            REQUIRE(gzipHeader[1] == 0x8b);
            REQUIRE(gzipHeader[2] == 0x08);

            /* string js{buffer.data(), buffer.size()};
            smatch match;
            REQUIRE(regex_search(js, match, regex("var webfront")));
            REQUIRE(regex_search(js, match, regex("major:0")));
            REQUIRE(regex_search(js, match, regex("minor:1")));
            REQUIRE(regex_search(js, match, regex("patch:1"))); */
        }
    }
}
}
