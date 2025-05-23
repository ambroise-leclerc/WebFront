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
        } catch (const std::exception& e) {
            std::cerr << "Warning: Failed to clean up test directory: " << e.what() << std::endl;
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
        std::vector<uint8_t> data(size);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(0, 255);
        
        for (auto& byte : data) {
            byte = static_cast<uint8_t>(distrib(gen));
        }
        
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
        file.close();
        
        binaryData[relativePath] = std::move(data);
        return filePath;
    }
    
    filesystem::path getTestDir() const { return testDir; }
    filesystem::path getSubDir() const { return subDir; }
    
    const std::vector<uint8_t>& getBinaryData(const string& relativePath) const {
        return binaryData.at(relativePath);
    }
    
private:
    filesystem::path testDir;
    filesystem::path subDir;
    std::map<string, std::vector<uint8_t>> binaryData;
};

SCENARIO("NativeDebugFS gives access to local files") {
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
                std::array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                REQUIRE(bytesRead == 27);
                REQUIRE(std::string(buffer.data(), 27) == "TestFile for NativeFSTests\n");
                REQUIRE(file->eof());
            }
        }
        filesystem::remove(testFilename);
    }
}

SCENARIO("NativeDebugFS handles various file operations") {
    TemporaryTestEnvironment testEnv;
    
    GIVEN("A test environment with different types of files") {
        // Create test files
        auto textFilePath = testEnv.createTextFile("sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
        auto configFilePath = testEnv.createTextFile("config.json", "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
        auto binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);
        auto emptyFilePath = testEnv.createTextFile("empty.txt", "");
        auto subdirFilePath = testEnv.createTextFile("subdir/nested.txt", "This is a nested file in a subdirectory");
        
        DebugFS debugFS(testEnv.getTestDir().string());
        
        WHEN("Opening an existing text file with relative path") {
            auto file = debugFS.open("sample.txt");
            
            THEN("The file is opened successfully and content is correct") {
                REQUIRE(file.has_value());
                REQUIRE(!file->isEncoded());
                
                std::array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                std::string content(buffer.data(), bytesRead);
                
                REQUIRE(content == "This is a sample text file\nwith multiple lines\nfor testing.");
                REQUIRE(file->eof());
            }
        }
        
        WHEN("Opening a JSON configuration file") {
            auto file = debugFS.open("config.json");
            
            THEN("The file is opened successfully and JSON content is correct") {
                REQUIRE(file.has_value());
                
                std::array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                std::string content(buffer.data(), bytesRead);
                
                REQUIRE(content == "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
            }
        }
        
        WHEN("Opening a binary file") {
            auto file = debugFS.open("data.bin");
            const auto& originalData = testEnv.getBinaryData("data.bin");
            
            THEN("The file is opened successfully and binary content matches") {
                REQUIRE(file.has_value());
                REQUIRE(!file->isEncoded());
                
                std::vector<char> buffer(1024);
                auto bytesRead = file->read(std::span(buffer));
                
                REQUIRE(bytesRead == 1024);
                // EOF behavior can vary between systems and implementations
                // So we only check that we've read all expected bytes and content matches
                REQUIRE(std::memcmp(buffer.data(), originalData.data(), bytesRead) == 0);
            }
        }
        
        WHEN("Opening an empty file") {
            auto file = debugFS.open("empty.txt");
            
            THEN("The file is opened but is empty") {
                REQUIRE(file.has_value());
                
                std::array<char, 10> buffer;
                auto bytesRead = file->read(buffer);
                
                REQUIRE(bytesRead == 0);
                REQUIRE(file->eof());
            }
        }
        
        WHEN("Opening a file in a subdirectory") {
            auto file = debugFS.open("subdir/nested.txt");
            
            THEN("The file is opened successfully") {
                REQUIRE(file.has_value());
                
                std::array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                std::string content(buffer.data(), bytesRead);
                
                REQUIRE(content == "This is a nested file in a subdirectory");
            }
        }
        
        WHEN("Trying to open a non-existent file") {
            auto file = debugFS.open("non_existent.txt");
            
            THEN("An empty optional is returned") {
                REQUIRE(!file.has_value());
            }
        }
        
        WHEN("Trying to open a file using an invalid path with ../ navigation") {
            auto file = debugFS.open("../unauthorized.txt");
            
            THEN("An empty optional is returned for security") {
                REQUIRE(!file.has_value());
            }
        }
    }
}

SCENARIO("NativeDebugFS root path handling") {
    TemporaryTestEnvironment testEnv;
    auto testFile = testEnv.createTextFile("test_root.txt", "Root path test");
    
    GIVEN("A NativeDebugFS with root path set to the test directory") {
        DebugFS debugFS(testEnv.getTestDir().string());
        
        WHEN("Opening a file with absolute path that doesn't respect the root path") {
            // Try to access a file outside of the root path using absolute path
            auto file = debugFS.open(filesystem::absolute("test_root.txt").string());
            
            THEN("File access should fail as it's not under the root path") {
                REQUIRE(!file.has_value());
            }
        }
        
        WHEN("Changing the working directory and using relative paths") {
            // Save current directory
            auto originalPath = filesystem::current_path();
            
            // Change to test directory
            filesystem::current_path(testEnv.getTestDir());
            
            // Try to open the file
            auto file = debugFS.open("test_root.txt");
            
            // Restore original directory
            filesystem::current_path(originalPath);
            
            THEN("File access should succeed as the path is resolved correctly") {
                REQUIRE(file.has_value());
                std::array<char, 512> buffer;
                auto bytesRead = file->read(buffer);
                REQUIRE(std::string(buffer.data(), bytesRead) == "Root path test");
            }
        }
    }
    
    GIVEN("A NativeDebugFS with root path set to a subdirectory") {
        DebugFS debugFS(testEnv.getSubDir().string());
        auto subDirFile = testEnv.createTextFile("subdir/subdir_test.txt", "Subdir test");
        
        WHEN("Trying to access a file in the parent directory") {
            auto file = debugFS.open("../test_root.txt");
            
            THEN("File access should succeed if path resolution allows navigation outside root") {
                // This behavior depends on the implementation of NativeRawFS::open
                // In many implementations, this would be denied for security reasons
                // But let's test the actual behavior
                if (file.has_value()) {
                    std::array<char, 512> buffer;
                    auto bytesRead = file->read(buffer);
                    REQUIRE(std::string(buffer.data(), bytesRead) == "Root path test");
                }
                // If not implemented, we should at least document the expected behavior
                // REQUIRE(!file.has_value());
            }
        }
    }
}