#include <system/ReactFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

// Deliberately using large stack buffers in tests
#pragma warning(disable: 6262)

using namespace std;
using namespace webfront;

SCENARIO("ReactFS provides React library access") {
    GIVEN("A ReactFS") {
        using FS = fs::ReactFS;

        WHEN("Requesting react.js") {
            auto file = FS::open("react@18/umd/react.production.min.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 4000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 3761);
            }
        }
        WHEN("Requesting react-dom.js") {
            auto file = FS::open("react-dom@18/umd/react-dom.production.min.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "br");
                array<uint8_t, 40000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(readSize == 37235);
            }
        }
    }
}