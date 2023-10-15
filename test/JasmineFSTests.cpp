#include <system/JasmineFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

using namespace std;
using namespace webfront;

SCENARIO("JasmineFS provides Jasmine testing library") {
    GIVEN("A JasmineFS") {
        using FS = fs::JasmineFS;

        WHEN("Requesting jasmine_favicon.png") {
            auto file = FS::open("jasmine/4.6.0/jasmine_favicon.png");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 2000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 1162);
            }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/jasmine.css");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 7000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 6409);
            }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/jasmine.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 55000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 50056);
            }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/jasmine-html.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 6000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 5166);
            }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/boot0.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 1000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 990);
            }
        }
        WHEN("Requesting jasmine") {
            auto file = FS::open("jasmine/4.6.0/boot1.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 2000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 1475);
            }
        }
    }
}