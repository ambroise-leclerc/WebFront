#include <system/Filesystem.hpp>
#include <tooling/HexDump.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

using namespace std;
using namespace webfront;


SCENARIO("IndexFileStystem provides basic files for browser support") {
    GIVEN("An IndexFS") {
        using FS = webfront::filesystem::IndexFS;
        WHEN("Requesting index.html") {
            auto indexFile = FS::open("index.html");
            THEN("A correct HTML5 file with the WebFront.js script inclusion is returned") {
                REQUIRE(indexFile.has_value());
                array<char, 1024> buffer;
                indexFile->read(buffer);

                string html{buffer.data(), buffer.size()};
                REQUIRE(html.starts_with("<!DOCTYPE html>"));

                smatch match;
                REQUIRE(regex_search(html, match, regex("<script.*src=\"WebFront.js\".*>")));
            }
        }
        WHEN("Requesting favicon.ico") {
            auto faviconFile = FS::open("favicon.ico");
            THEN("correct data is returned") {
                REQUIRE(faviconFile.has_value());

                array<uint8_t, 1024> buffer;
                auto readSize = faviconFile->read(buffer);
                REQUIRE(readSize == 766);

                array<uint8_t, 16> head{0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x20,
                                             0x10, 0x00, 0x01, 0x00, 0x04, 0x00, 0xE8, 0x02};
                array<uint8_t, 16> tail{0X00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                             0x80, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x07};
                REQUIRE(equal(head.begin(), head.end(), buffer.begin()));
                REQUIRE(equal(tail.begin(), tail.end(), buffer.begin() + readSize - tail.size()));
            }
        }
        WHEN("Requesting WebFront.js") { auto webfrontJSFile = FS::open("WebFront.js");
            THEN("correct data is returned") { REQUIRE(webfrontJSFile.has_value());
                array<char, 128> buffer;
                
                webfrontJSFile->read(buffer);
                cout << "WebfrontJS : " << std::string(buffer.data(), buffer.size()) << "\n";
                

            }
        }
    }
}
