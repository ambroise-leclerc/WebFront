#include <system/Filesystem.hpp>
#include <tooling/HexDump.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <iostream>

using namespace std;
using namespace webfront;


SCENARIO("IndexFileStystem provides basic files for browser support") {
    GIVEN("An IndexFS") {
        using FS = webfront::filesystem::IndexFS;
        WHEN("Requesting index.html") {
            auto indexFile = FS::open("index.html");
            THEN("correct data is returned") {
                REQUIRE(indexFile.has_value());

            }
        }
        WHEN("Requesting favicon.ico") {
            auto faviconFile = FS::open("favicon.ico");
            THEN("correct data is returned") {
                REQUIRE(faviconFile.has_value());

                std::array<uint8_t, 1024> buffer;
                auto readSize = faviconFile->read(buffer);
                REQUIRE(readSize == 766);

                cout << utils::hexDump(buffer) << "\n";
                std::array<uint8_t, 16> head{0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x20,
                                             0x10, 0x00, 0x01, 0x00, 0x04, 0x00, 0xE8, 0x02};
                std::array<uint8_t, 16> tail{0X00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
                                             0x80, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x07};
                REQUIRE(equal(head.begin(), head.end(), buffer.begin()));
                REQUIRE(equal(tail.begin(), tail.end(), buffer.begin() + readSize - tail.size()));
            }
        }
    }
}