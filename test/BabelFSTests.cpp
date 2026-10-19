#include <system/BabelFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>
#include <regex>
#include <string_view>

// Deliberately using large stack buffers in tests
#ifdef _MSC_VER
#pragma warning(disable: 6262)
#endif

using namespace std;
using namespace webfront;

SCENARIO("BabelFS provides Babel/standalone library access") {
    GIVEN("A BabelFS") {
        using FS = fs::BabelFS;

        WHEN("Requesting babel.js") {
            auto file = FS::open("babel-standalone@7.22.5/babel.min.js");
            THEN("correct data is returned") {
                REQUIRE(file.has_value());
                REQUIRE(file->isEncoded());
                REQUIRE(file->getEncoding() == "gzip");
                array<uint8_t, 597000> buffer;
                auto readSize = file->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                REQUIRE(buffer[0] == 0x1f);
                REQUIRE(buffer[1] == 0x8b);
                REQUIRE(readSize == 596730);
            }
        }
    }
}
