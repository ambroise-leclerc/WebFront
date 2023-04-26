#include <system/JasmineFS.hpp>
#include <tooling/HexDump.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

using namespace std;
using namespace webfront;

SCENARIO("IndexFileSystem provides basic files for browser support") {
    GIVEN("A JasmineFS") {
        using FS = webfront::filesystem::JasmineFS;

        WHEN("Requesting jasmine_favicon.png") {
            auto file = FS::open("jasmine/4.6.0/jasmine_favicon.png");
            THEN("correct data is returned") { REQUIRE(file.has_value()); }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/jasmine.css");
            THEN("correct data is returned") { REQUIRE(file.has_value()); }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/jasmine.js");
            THEN("correct data is returned") { REQUIRE(file.has_value()); }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/jasmine-html.js");
            THEN("correct data is returned") { REQUIRE(file.has_value()); }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/boot0.js");
            THEN("correct data is returned") { REQUIRE(file.has_value()); }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/boot1.js");
            THEN("correct data is returned") { REQUIRE(file.has_value()); }
        }
    }
}