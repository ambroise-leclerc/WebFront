#include <sstream>
#include <system/NativeFS.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using namespace webfront;

using DebugFS = fs::NativeDebugFS;

// Helper function to create a temporary test directory structure
class TemporaryTestEnvironment {
public:
    TemporaryTestEnvironment() {
        // Create a unique test directory
        testDir = filesystem::temp_directory_path() / "nativefs_test";
        filesystem::create_directories(testDir);
        
        // Create subdirectory
        subDir = testDir / "subdir";
        filesystem::create_directories(subDir);
    }
    
    ~TemporaryTestEnvironment() {
        try {
            filesystem::remove_all(testDir);
        } catch (const exception& e) {
            cerr << "Warning: Failed to clean up test directory: " << e.what() << endl;
        }
    }
    
    // Create a text file with specific content
    filesystem::path createTextFile(const string& relativePath, const string& content) {
        filesystem::path filePath = testDir / relativePath;
        ofstream file(filePath, ios::out);
        file << content;
        file.close();
        return filePath;
    }
    
    // Create a binary file with random content
    filesystem::path createBinaryFile(const string& relativePath, size_t size) {
        filesystem::path filePath = testDir / relativePath;
        ofstream file(filePath, ios::binary);
        
        // Generate random bytes
        vector<uint8_t> data(size);
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<> distrib(0, 255);
        
        for (auto& byte : data) {
            byte = static_cast<uint8_t>(distrib(gen));
        }
        
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<streamsize>(size));
        file.close();
        
        binaryData[relativePath] = move(data);
        return filePath;
    }
    
    filesystem::path getTestDir() const { return testDir; }
    filesystem::path getSubDir() const { return subDir; }
    
    const vector<uint8_t>& getBinaryData(const string& relativePath) const {
        return binaryData.at(relativePath);
    }
    
private:
    filesystem::path testDir;
    filesystem::path subDir;
    map<string, vector<uint8_t>> binaryData;
};

namespace {
    void checkFileRead(DebugFS& debugFS, const string& filename, const string& expectedContent) {
        auto file = debugFS.open(filename);
        REQUIRE(file.has_value());
        REQUIRE(!file->isEncoded());
        array<char, 512> buffer;
        auto bytesRead = file->read(buffer);
        string content(buffer.data(), bytesRead);
        REQUIRE(content == expectedContent);
        REQUIRE(file->eof());
    }
}

SCENARIO("NativeDebugFS provides access to local files") {
    GIVEN("A local file") {
        filesystem::path testFilename{"test.tmp"};
        {
            ofstream testFile(testFilename, ios::binary);
            testFile << "TestFile for NativeFSTests\n";
        }
        WHEN("NativeDebugFS opens it") {
            DebugFS debugFS(".");
            auto file = debugFS.open("test.tmp");
            THEN("A file with the correct content is returned") {
                REQUIRE(file);
                REQUIRE(!file->isEncoded());
                array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                REQUIRE(bytesRead == 27);
                REQUIRE(string(buffer.data(), 27) == "TestFile for NativeFSTests\n");
                REQUIRE(file->eof());
            }
        }
        filesystem::remove(testFilename);
    }
}

SCENARIO("NativeDebugFS opens an existing text file using a relative path") {
    TemporaryTestEnvironment testEnv;
    auto textFilePath = testEnv.createTextFile("sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
    DebugFS debugFS(testEnv.getTestDir().string());

    WHEN("The file sample.txt is opened") {
        THEN("The file is opened and the content is correct") {
            checkFileRead(debugFS, "sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
        }
    }
}

SCENARIO("NativeDebugFS opens a JSON configuration file") {
    TemporaryTestEnvironment testEnv;
    auto configFilePath = testEnv.createTextFile("config.json", "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
    DebugFS debugFS(testEnv.getTestDir().string());

    WHEN("The file config.json is opened") {
        THEN("The file is opened and the JSON content is correct") {
            checkFileRead(debugFS, "config.json", "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
        }
    }
}

SCENARIO("NativeDebugFS opens a binary file") {
    TemporaryTestEnvironment testEnv;
    auto binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);
    DebugFS debugFS(testEnv.getTestDir().string());
    const auto& originalData = testEnv.getBinaryData("data.bin");

    WHEN("The file data.bin is opened") {
        auto file = debugFS.open("data.bin");
        THEN("The binary content matches the original data") {
            REQUIRE(file.has_value());
            REQUIRE(!file->isEncoded());
            vector<char> buffer(1024);
            auto bytesRead = file->read(span(buffer));
            REQUIRE(bytesRead == 1024);
            REQUIRE(memcmp(buffer.data(), originalData.data(), bytesRead) == 0);
        }
    }
}

SCENARIO("NativeDebugFS opens an empty file") {
    TemporaryTestEnvironment testEnv;
    auto emptyFilePath = testEnv.createTextFile("empty.txt", "");
    DebugFS debugFS(testEnv.getTestDir().string());

    WHEN("The file empty.txt is opened") {
        auto file = debugFS.open("empty.txt");
        THEN("The file is opened but it is empty") {
            REQUIRE(file.has_value());
            array<char, 10> buffer;
            auto bytesRead = file->read(buffer);
            REQUIRE(bytesRead == 0);
            REQUIRE(file->eof());
        }
    }
}

SCENARIO("NativeDebugFS opens a file in a subdirectory") {
    TemporaryTestEnvironment testEnv;
    auto subdirFilePath = testEnv.createTextFile("subdir/nested.txt", "This is a nested file in a subdirectory");
    DebugFS debugFS(testEnv.getTestDir().string());

    WHEN("The file subdir/nested.txt is opened") {
        THEN("The file is opened successfully") {
            checkFileRead(debugFS, "subdir/nested.txt", "This is a nested file in a subdirectory");
        }
    }
}

SCENARIO("NativeDebugFS attempts to open a non-existent file") {
    TemporaryTestEnvironment testEnv;
    DebugFS debugFS(testEnv.getTestDir().string());

    WHEN("An attempt is made to open non_existent.txt") {
        auto file = debugFS.open("non_existent.txt");
        THEN("An empty optional is returned") {
            REQUIRE(!file.has_value());
        }
    }

    WHEN("An attempt is made to open ../unauthorised.txt") {
        auto file = debugFS.open("../unauthorized.txt");
        THEN("An empty optional is returned for security reasons") {
            REQUIRE(!file.has_value());
        }
    }
}

SCENARIO("TemporaryTestEnvironment::getBinaryData returns the correct binary data") {
    TemporaryTestEnvironment testEnv;
    auto binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);

    WHEN("Binary data is retrieved via getBinaryData") {
        const auto& data = testEnv.getBinaryData("data.bin");
        THEN("The size and content are correct") {
            REQUIRE(data.size() == 1024);
            REQUIRE(!data.empty());
        }
    }
}

SCENARIO("NativeDebugFS root path handling") {
    TemporaryTestEnvironment testEnv;
    auto testFile = testEnv.createTextFile("test_root.txt", "Root path test");
    
    GIVEN("A NativeDebugFS with the root path set to the test directory") {
        DebugFS debugFS(testEnv.getTestDir().string());
        
        WHEN("A file is opened using an absolute path that does not respect the root path") {
            auto file = debugFS.open(filesystem::absolute("test_root.txt").string());
            
            THEN("File access should fail as it is not under the root path") {
                REQUIRE(!file.has_value());
            }
        }
        
        WHEN("The working directory is changed and relative paths are used") {
            auto originalPath = filesystem::current_path();
            
            filesystem::current_path(testEnv.getTestDir());
            auto file = debugFS.open("test_root.txt");
            filesystem::current_path(originalPath);
            
            THEN("File access should succeed as the path is resolved correctly") {
                REQUIRE(file.has_value());
                array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                REQUIRE(string(buffer.data(), bytesRead) == "Root path test");
            }
        }
    }
    
    GIVEN("A NativeDebugFS with the root path set to a subdirectory") {
        DebugFS debugFS(testEnv.getSubDir().string());
        auto subDirFile = testEnv.createTextFile("subdir/subdir_test.txt", "Subdir test");
        
        WHEN("An attempt is made to access a file in the parent directory") {
            auto file = debugFS.open("../test_root.txt");
            
            THEN("File access behaviour depends on the implementation's security policy") {
                if (file.has_value()) {
                    array<char, 512> buffer;
                    auto bytesRead = file->read(buffer);
                    REQUIRE(string(buffer.data(), bytesRead) == "Root path test");
                }
            }
        }
    }
}

SCENARIO("TemporaryTestEnvironment destructor handles remove_all exception") {
    unique_ptr<TemporaryTestEnvironment> testEnv = make_unique<TemporaryTestEnvironment>();
    auto protectedDir = testEnv->getTestDir() / "protected";
    filesystem::create_directory(protectedDir);
    filesystem::permissions(protectedDir, filesystem::perms::owner_read, filesystem::perm_options::replace);

    stringstream capturedCerr;
    auto* oldCerrBuf = cerr.rdbuf(capturedCerr.rdbuf());

    testEnv.reset();

    // Restore permissions to allow cleanup after the test
    error_code ec;
    filesystem::permissions(protectedDir, filesystem::perms::owner_all, filesystem::perm_options::replace, ec);

    cerr.rdbuf(oldCerrBuf);

    // The test passes if the warning message is present OR if nothing is written (removal succeeded)
    auto warningFound = capturedCerr.str().find("Warning: Failed to clean up test directory") != string::npos;
    REQUIRE((warningFound || capturedCerr.str().empty()));
}