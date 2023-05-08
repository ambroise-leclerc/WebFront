#include <sstream>
#include <system/NativeFS.hpp>

#include <doctest/doctest.h>

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
            auto file = DebugFS::open("test.tmp");
            THEN("A file with the correct content is returned") {
                REQUIRE(file);
                // REQUIRE(file.value().get() == 'T');
                stringstream buffer;
                buffer << file.value().rdbuf();
                REQUIRE(buffer.str() == "TestFile for NativeFSTests\n");
            }
        }
    }
}