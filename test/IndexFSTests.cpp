#include <system/IndexFS.hpp>
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
                if (!indexFile->isEncoded()) {
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
                array<uint8_t, 1024> buffer;
                auto readSize = faviconFile->read(reinterpret_cast<char*>(buffer.data()), buffer.size()).gcount();
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
                readSize = faviconFile->read(reinterpret_cast<char*>(buffer.data()), buffer.size()).gcount();
                REQUIRE(readSize == 0);
            }
        }
        WHEN("Requesting WebFront.js") {
            auto webfrontJSFile = FS::open("WebFront.js");
            THEN("Wbefront.js V0.0.1 content should be returned") {
                REQUIRE(webfrontJSFile.has_value());

                REQUIRE(webfrontJSFile->isEncoded());
                REQUIRE(webfrontJSFile->getEncoding() ==
                        "gzip"); // Only gzip encoding should be used for mandatory file such as WebFront.js since android
                                 // webviews does not support br encoding

                array<uint8_t, 10> gzipHeader;
                webfrontJSFile->read(reinterpret_cast<char*>(gzipHeader.data()), gzipHeader.size());

                REQUIRE(gzipHeader[0] == 0x1f);
                REQUIRE(gzipHeader[1] == 0x8b);
                REQUIRE(gzipHeader[2] == 0x08);
            }
        }
    }
}
