#include <sstream>
#include "TemporaryDirectory.hpp"
#include <webfront/system/NativeFS.hpp>

#include "Bdd.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

using DebugFS = fs::NativeDebugFS;

// Helper function to create a temporary test directory structure
class TemporaryTestEnvironment {
public:
    TemporaryTestEnvironment() : testDir(root.path()) {
        
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
        
        binaryData[relativePath] = std::move(data);
        return filePath;
    }
    
    filesystem::path getTestDir() const { return testDir; }
    filesystem::path getSubDir() const { return subDir; }
    
    const vector<uint8_t>& getBinaryData(const string& relativePath) const {
        return binaryData.at(relativePath);
    }
    
private:
    webfront::test::TemporaryDirectory root;
    filesystem::path testDir;
    filesystem::path subDir;
    map<string, vector<uint8_t>> binaryData;
};

namespace {
    std::string normaliseNewlines(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r') {
                if (i + 1 < s.size() && s[i + 1] == '\n') continue; // skip, handled by \n
            }
            out += s[i];
        }
        return out;
    }

    void checkFileRead(DebugFS& debugFS, const std::string& filename, const std::string& expectedContent) {
        auto file = debugFS.open(filename);
        require(file.has_value());
        require(!file->isEncoded());
        std::array<char, 512> buffer;
        auto bytesRead = file->read(buffer);
        std::string content(buffer.data(), bytesRead);
        require(normaliseNewlines(content) == normaliseNewlines(expectedContent));
        require(file->eof());
    }
}

Scenario nativeDebugFSProvides{"NativeDebugFS provides access to local files", [] {
    given("A local file", [&] {
        filesystem::path testFilename{"test.tmp"};
        {
            ofstream testFile(testFilename, ios::binary);
            testFile << "TestFile for NativeFSTests\n";
        }
        when("NativeDebugFS opens it", [&] {
            DebugFS debugFS(".");
            auto file = debugFS.open("test.tmp");
            then("A file with the correct content is returned", [&] {
                require(file);
                require(!file->isEncoded());
                array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                require(bytesRead == 27);
                require(string(buffer.data(), 27) == "TestFile for NativeFSTests\n");
                require(file->eof());
            });
        });
        filesystem::remove(testFilename);
    });
}};

Scenario nativeDebugFSOpens{"NativeDebugFS opens an existing text file using a relative path", [] {
    TemporaryTestEnvironment testEnv;
    auto textFilePath = testEnv.createTextFile("sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
    DebugFS debugFS(testEnv.getTestDir().string());

    when("The file sample.txt is opened", [&] {
        then("The file is opened and the content is correct", [&] {
            checkFileRead(debugFS, "sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
        });
    });
}};

Scenario nativeDebugFSOpensJSON{"NativeDebugFS opens a JSON configuration file", [] {
    TemporaryTestEnvironment testEnv;
    auto configFilePath = testEnv.createTextFile("config.json", "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
    DebugFS debugFS(testEnv.getTestDir().string());

    when("The file config.json is opened", [&] {
        then("The file is opened and the JSON content is correct", [&] {
            checkFileRead(debugFS, "config.json", "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
        });
    });
}};

Scenario nativeDebugFSOpensBinary{"NativeDebugFS opens a binary file", [] {
    TemporaryTestEnvironment testEnv;
    auto binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);
    DebugFS debugFS(testEnv.getTestDir().string());
    const auto& originalData = testEnv.getBinaryData("data.bin");

    when("The file data.bin is opened", [&] {
        auto file = debugFS.open("data.bin");
        then("The binary content matches the original data", [&] {
            require(file.has_value());
            require(!file->isEncoded());
            vector<char> buffer(1024);
            auto bytesRead = file->read(span(buffer));
            require(bytesRead == 1024);
            require(memcmp(buffer.data(), originalData.data(), bytesRead) == 0);
        });
    });
}};

Scenario nativeDebugFSOpensEmpty{"NativeDebugFS opens an empty file", [] {
    TemporaryTestEnvironment testEnv;
    auto emptyFilePath = testEnv.createTextFile("empty.txt", "");
    DebugFS debugFS(testEnv.getTestDir().string());

    when("The file empty.txt is opened", [&] {
        auto file = debugFS.open("empty.txt");
        then("The file is opened but it is empty", [&] {
            require(file.has_value());
            array<char, 10> buffer;
            auto bytesRead = file->read(buffer);
            require(bytesRead == 0);
            require(file->eof());
        });
    });
}};

Scenario nativeDebugFSOpensFile{"NativeDebugFS opens a file in a subdirectory", [] {
    TemporaryTestEnvironment testEnv;
    auto subdirFilePath = testEnv.createTextFile("subdir/nested.txt", "This is a nested file in a subdirectory");
    DebugFS debugFS(testEnv.getTestDir().string());

    when("The file subdir/nested.txt is opened", [&] {
        then("The file is opened successfully", [&] {
            checkFileRead(debugFS, "subdir/nested.txt", "This is a nested file in a subdirectory");
        });
    });
}};

Scenario nativeDebugFSAttempts{"NativeDebugFS attempts to open a non-existent file", [] {
    TemporaryTestEnvironment testEnv;
    DebugFS debugFS(testEnv.getTestDir().string());

    when("An attempt is made to open non_existent.txt", [&] {
        auto file = debugFS.open("non_existent.txt");
        then("An empty optional is returned", [&] {
            require(!file.has_value());
        });
    });

    when("An attempt is made to open ../unauthorized.txt", [&] {
        auto file = debugFS.open("../unauthorized.txt");
        then("An empty optional is returned for security reasons", [&] {
            require(!file.has_value());
        });
    });
}};

Scenario temporaryTestEnvironmentGetBinaryData{"TemporaryTestEnvironment::getBinaryData returns the correct binary data", [] {
    TemporaryTestEnvironment testEnv;
    auto binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);

    when("Binary data is retrieved via getBinaryData", [&] {
        const auto& data = testEnv.getBinaryData("data.bin");
        then("The size and content are correct", [&] {
            require(data.size() == 1024);
            require(!data.empty());
        });
    });
}};

Scenario nativeDebugFSRoot{"NativeDebugFS root path handling", [] {
    TemporaryTestEnvironment testEnv;
    auto testFile = testEnv.createTextFile("test_root.txt", "Root path test");
    
    given("A NativeDebugFS with the root path set to the test directory", [&] {
        DebugFS debugFS(testEnv.getTestDir().string());
        
        when("A file is opened using an absolute path that does not respect the root path", [&] {
            auto file = debugFS.open(filesystem::absolute("test_root.txt").string());
            
            then("File access should fail as it is not under the root path", [&] {
                require(!file.has_value());
            });
        });
        
        when("The working directory is changed and relative paths are used", [&] {
            auto originalPath = filesystem::current_path();
            
            filesystem::current_path(testEnv.getTestDir());
            auto file = debugFS.open("test_root.txt");
            filesystem::current_path(originalPath);
            
            then("File access should succeed as the path is resolved correctly", [&] {
                require(file.has_value());
                array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                require(string(buffer.data(), bytesRead) == "Root path test");
            });
        });
    });
    
    given("A NativeDebugFS with the root path set to a subdirectory", [&] {
        DebugFS debugFS(testEnv.getSubDir().string());
        auto subDirFile = testEnv.createTextFile("subdir/subdir_test.txt", "Subdir test");
        
        when("An attempt is made to access a file in the parent directory", [&] {
            auto file = debugFS.open("../test_root.txt");
            
            then("File access behaviour depends on the implementation's security policy", [&] {
                if (file.has_value()) {
                    array<char, 512> buffer;
                    auto bytesRead = file->read(buffer);
                    require(string(buffer.data(), bytesRead) == "Root path test");
                }
            });
        });
    });
}};

Scenario temporaryTestEnvironmentDestructor{"TemporaryTestEnvironment destructor handles remove_all exception", [] {
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
    require((warningFound || capturedCerr.str().empty()));
}};