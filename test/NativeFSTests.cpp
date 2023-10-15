#include <sstream>
#include <system/NativeFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace std;
using namespace webfront;

using DebugFS = fs::NativeDebugFS;

SCENARIO("NativeDebugFS give access to local files") {
    GIVEN("A local file") {
        filesystem::path testFilename{"test.tmp"};
        {
            ofstream testFile(testFilename, ios::binary);
            testFile << "TestFile for NativeFSTests\n";
        }
        WHEN("NativeDebugFS open it") {
            DebugFS debugFS(".");
            auto file = debugFS.open("test.tmp");
            THEN("A file with the correct content is returned") {
                REQUIRE(file);
                REQUIRE(!file->isEncoded());
                std::array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                REQUIRE(bytesRead == 27);
                REQUIRE(std::string(buffer.data(), 27) == "TestFile for NativeFSTests\n");
                REQUIRE(file->eof());
            }
        }
    }
}