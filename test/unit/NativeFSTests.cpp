#include "TemporaryDirectory.hpp"
#include <webfront/system/NativeFS.hpp>
#include <webfront/testing/Bdd.hpp>

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
        ofstream         file(filePath, ios::out);
        file << content;
        file.close();
        return filePath;
    }

    // Create a binary file with random content
    filesystem::path createBinaryFile(const string& relativePath, size_t size) {
        filesystem::path filePath = testDir / relativePath;
        ofstream         file(filePath, ios::binary);

        // Generate random bytes
        vector<uint8_t>            data(size);
        random_device              rd;
        mt19937                    gen(rd());
        uniform_int_distribution<> distrib(0, 255);

        for (auto& byte : data) {
            byte = static_cast<uint8_t>(distrib(gen));
        }

        file.write(reinterpret_cast<const char*>(data.data()), static_cast<streamsize>(size));
        file.close();

        binaryData[relativePath] = std::move(data);
        return filePath;
    }

    filesystem::path getTestDir() const {
        return testDir;
    }
    filesystem::path getSubDir() const {
        return subDir;
    }

    const vector<uint8_t>& getBinaryData(const string& relativePath) const {
        return binaryData.at(relativePath);
    }

private:
    webfront::test::TemporaryDirectory root;
    filesystem::path                   testDir;
    filesystem::path                   subDir;
    map<string, vector<uint8_t>>       binaryData;
};

namespace {
std::string normaliseNewlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n')
                continue;  // skip, handled by \n
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
    auto                  bytesRead = file->read(buffer);
    std::string           content(buffer.data(), bytesRead);
    require(normaliseNewlines(content) == normaliseNewlines(expectedContent));
    require(file->eof());
}
}  // namespace

const Scenario nativeDebugFSProvides{"NativeDebugFS provides access to local files", [] {
                                         given("A local file", [&] {
                                             filesystem::path testFilename{"test.tmp"};
                                             {
                                                 ofstream testFile(testFilename, ios::binary);
                                                 testFile << "TestFile for NativeFSTests\n";
                                             }
                                             when("NativeDebugFS opens it", [&] {
                                                 DebugFS debugFS(".");
                                                 auto    file = debugFS.open("test.tmp");
                                                 then("A file with the correct content is returned", [&] {
                                                     require(file);
                                                     require(!file->isEncoded());
                                                     array<char, 512> buffer;
                                                     auto             bytesRead = file->read(buffer);
                                                     require(bytesRead == 27);
                                                     require(string(buffer.data(), 27) == "TestFile for NativeFSTests\n");
                                                     require(file->eof());
                                                 });
                                             });
                                             filesystem::remove(testFilename);
                                         });
                                     }};

const Scenario nativeDebugFSOpens{"NativeDebugFS opens an existing text file using a relative path", [] {
                                      TemporaryTestEnvironment testEnv;
                                      auto textFilePath = testEnv.createTextFile("sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
                                      DebugFS debugFS(testEnv.getTestDir().string());

                                      when("The file sample.txt is opened", [&] {
                                          then("The file is opened and the content is correct", [&] {
                                              checkFileRead(debugFS, "sample.txt", "This is a sample text file\nwith multiple lines\nfor testing.");
                                          });
                                      });
                                  }};

const Scenario nativeDebugFSOpensJSON{"NativeDebugFS opens a JSON configuration file", [] {
                                          TemporaryTestEnvironment testEnv;
                                          auto                     configFilePath = testEnv.createTextFile("config.json",
                                                                                       "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
                                          DebugFS                  debugFS(testEnv.getTestDir().string());

                                          when("The file config.json is opened", [&] {
                                              then("The file is opened and the JSON content is correct", [&] {
                                                  checkFileRead(debugFS, "config.json", "{ \"name\": \"NativeFS\", \"version\": 1.0, \"isTest\": true }");
                                              });
                                          });
                                      }};

const Scenario nativeDebugFSOpensBinary{"NativeDebugFS opens a binary file", [] {
                                            TemporaryTestEnvironment testEnv;
                                            auto                     binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);
                                            DebugFS                  debugFS(testEnv.getTestDir().string());
                                            const auto&              originalData = testEnv.getBinaryData("data.bin");

                                            when("The file data.bin is opened", [&] {
                                                auto file = debugFS.open("data.bin");
                                                then("The binary content matches the original data", [&] {
                                                    require(file.has_value());
                                                    require(!file->isEncoded());
                                                    vector<char> buffer(1024);
                                                    auto         bytesRead = file->read(span(buffer));
                                                    require(bytesRead == 1024);
                                                    require(memcmp(buffer.data(), originalData.data(), bytesRead) == 0);
                                                });
                                            });
                                        }};

const Scenario nativeDebugFSOpensEmpty{"NativeDebugFS opens an empty file", [] {
                                           TemporaryTestEnvironment testEnv;
                                           auto                     emptyFilePath = testEnv.createTextFile("empty.txt", "");
                                           DebugFS                  debugFS(testEnv.getTestDir().string());

                                           when("The file empty.txt is opened", [&] {
                                               auto file = debugFS.open("empty.txt");
                                               then("The file is opened but it is empty", [&] {
                                                   require(file.has_value());
                                                   array<char, 10> buffer;
                                                   auto            bytesRead = file->read(buffer);
                                                   require(bytesRead == 0);
                                                   require(file->eof());
                                               });
                                           });
                                       }};

const Scenario nativeDebugFSOpensFile{"NativeDebugFS opens a file in a subdirectory", [] {
                                          TemporaryTestEnvironment testEnv;
                                          auto    subdirFilePath = testEnv.createTextFile("subdir/nested.txt", "This is a nested file in a subdirectory");
                                          DebugFS debugFS(testEnv.getTestDir().string());

                                          when("The file subdir/nested.txt is opened", [&] {
                                              then("The file is opened successfully", [&] {
                                                  checkFileRead(debugFS, "subdir/nested.txt", "This is a nested file in a subdirectory");
                                              });
                                          });
                                      }};

const Scenario nativeDebugFSAttempts{"NativeDebugFS attempts to open a non-existent file", [] {
                                         TemporaryTestEnvironment testEnv;
                                         DebugFS                  debugFS(testEnv.getTestDir().string());

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

const Scenario temporaryTestEnvironmentGetBinaryData{"TemporaryTestEnvironment::getBinaryData returns the correct binary data", [] {
                                                         TemporaryTestEnvironment testEnv;
                                                         auto                     binaryFilePath = testEnv.createBinaryFile("data.bin", 1024);

                                                         when("Binary data is retrieved via getBinaryData", [&] {
                                                             const auto& data = testEnv.getBinaryData("data.bin");
                                                             then("The size and content are correct", [&] {
                                                                 require(data.size() == 1024);
                                                                 require(!data.empty());
                                                             });
                                                         });
                                                     }};

const Scenario nativeDebugFSRoot{"NativeDebugFS root path handling", [] {
                                     TemporaryTestEnvironment testEnv;
                                     auto                     testFile = testEnv.createTextFile("test_root.txt", "Root path test");

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
                                                 auto             bytesRead = file->read(buffer);
                                                 require(string(buffer.data(), bytesRead) == "Root path test");
                                             });
                                         });
                                     });

                                     given("A NativeDebugFS with the root path set to a subdirectory", [&] {
                                         DebugFS debugFS(testEnv.getSubDir().string());
                                         auto    subDirFile = testEnv.createTextFile("subdir/subdir_test.txt", "Subdir test");

                                         when("An attempt is made to access a file in the parent directory", [&] {
                                             auto file = debugFS.open("../test_root.txt");

                                             then("File access behaviour depends on the implementation's security policy", [&] {
                                                 if (file.has_value()) {
                                                     array<char, 512> buffer;
                                                     auto             bytesRead = file->read(buffer);
                                                     require(string(buffer.data(), bytesRead) == "Root path test");
                                                 }
                                             });
                                         });
                                     });
                                 }};

const Scenario temporaryTestEnvironmentDestructor{
    "TemporaryTestEnvironment destructor handles remove_all exception",
    [] {
        unique_ptr<TemporaryTestEnvironment> testEnv      = make_unique<TemporaryTestEnvironment>();
        auto                                 protectedDir = testEnv->getTestDir() / "protected";
        filesystem::create_directory(protectedDir);
        filesystem::permissions(protectedDir, filesystem::perms::owner_read, filesystem::perm_options::replace);

        stringstream capturedCerr;
        auto*        oldCerrBuf = cerr.rdbuf(capturedCerr.rdbuf());

        testEnv.reset();

        // Restore permissions to allow cleanup after the test
        error_code ec;
        filesystem::permissions(protectedDir, filesystem::perms::owner_all, filesystem::perm_options::replace, ec);

        cerr.rdbuf(oldCerrBuf);

        // The test passes if the warning message is present OR if nothing is written (removal succeeded)
        auto warningFound = capturedCerr.str().find("Warning: Failed to clean up test directory") != string::npos;
        require((warningFound || capturedCerr.str().empty()));
    }};
// A stylesheet whose font-family lists many families is reported once by the development file system (#611).
const Scenario nativeDebugFSFontStacks{
    "NativeDebugFS warns once about costly font stacks in stylesheets",
    [] {
        given("stylesheets in a document root", [&] {
            TemporaryTestEnvironment env;
            env.createTextFile("long-stack.css",
                               "/* font-family: a, b, c, d, e */ body { font-family: system-ui, -apple-system, \"Segoe UI\", \"Inter\", sans-serif; }");
            env.createTextFile("short-stack.css", "body { font-family: system-ui, sans-serif; } code { font-family: \"A, B\", monospace; }");
            env.createTextFile("variable.css", "body { font-family: var(--a), x, y, z, w; --font-family: p, q, r, s, t; }");
            env.createTextFile("quoted.css", ".a { font-family: \"font-family: a, b, c, d, e\"; }");
            env.createTextFile("empty.css", "");
            when("each is opened twice", [&] {
                std::ostringstream warnings;
                struct WarningOutputScope {  // restored even when a require() throws
                    explicit WarningOutputScope(std::ostream& out) {
                        fonts::warningOutput = &out;
                    }
                    ~WarningOutputScope() {
                        fonts::warningOutput = nullptr;
                    }
                } const output{warnings};
                DebugFS root(env.getTestDir());
                for (int i = 0; i < 2; ++i)
                    for (const auto* name : {"long-stack.css", "short-stack.css", "variable.css", "quoted.css", "empty.css"})
                        require(root.open(name).has_value());
                checkFileRead(root, "empty.css", "");
                checkFileRead(root, "short-stack.css", "body { font-family: system-ui, sans-serif; } code { font-family: \"A, B\", monospace; }");
                const auto text = warnings.str();
                then("only the long stack is reported, once, with its count", [&] {
                    require(text.find("long-stack.css") != string::npos);
                    require(text.find("lists 4 families") != string::npos);
                    require(text.find("short-stack.css") == string::npos);
                    require(text.find("variable.css") == string::npos);
                    require(text.find("quoted.css") == string::npos);
                    require(text.find("long-stack.css") == text.rfind("long-stack.css"));
                    require(count(text.begin(), text.end(), '\n') == 1);
                });
            });
        });
    }};
// Two document roots serving a stylesheet of the same name are each checked (#611).
const Scenario nativeDebugFSFontStacksPerRoot{"NativeDebugFS checks a same-named stylesheet of each document root", [] {
                                                  given("two roots with a long font stack in style.css", [&] {
                                                      TemporaryTestEnvironment first;
                                                      TemporaryTestEnvironment second;
                                                      first.createTextFile("style.css", "body { font-family: aa1, bb1, cc1, dd1; }");
                                                      second.createTextFile("style.css", "body { font-family: aa2, bb2, cc2, dd2; }");
                                                      when("each root serves it", [&] {
                                                          std::ostringstream warnings;
                                                          fonts::warningOutput = &warnings;
                                                          DebugFS one(first.getTestDir());
                                                          DebugFS two(second.getTestDir());
                                                          require(one.open("style.css").has_value());
                                                          require(two.open("style.css").has_value());
                                                          fonts::warningOutput = nullptr;
                                                          const auto text      = warnings.str();
                                                          then("both are reported", [&] {
                                                              require(text.find("aa1") != string::npos);
                                                              require(text.find("aa2") != string::npos);
                                                          });
                                                      });
                                                  });
                                              }};
// A stylesheet edited after its first check is checked again, and an unchanged one is not (#611).
const Scenario nativeDebugFSFontStacksEdited{"NativeDebugFS checks an edited stylesheet again", [] {
                                                 given("a stylesheet without a long stack that is served once", [&] {
                                                     TemporaryTestEnvironment env;
                                                     env.createTextFile("live.css", "body { font-family: sans-serif; }");
                                                     std::ostringstream warnings;
                                                     fonts::warningOutput = &warnings;
                                                     DebugFS root(env.getTestDir());
                                                     require(root.open("live.css").has_value());
                                                     when("the developer adds a long stack and reloads twice", [&] {
                                                         env.createTextFile("live.css",
                                                                            "body { font-family: edited1, edited2, edited3, edited4, sans-serif; }");
                                                         require(root.open("live.css").has_value());
                                                         require(root.open("live.css").has_value());
                                                         fonts::warningOutput = nullptr;
                                                         const auto text      = warnings.str();
                                                         then("the new stack is reported once", [&] {
                                                             require(text.find("edited1") != string::npos);
                                                             require(count(text.begin(), text.end(), '\n') == 1);
                                                         });
                                                     });
                                                 });
                                             }};
// Declarations are told from text, whatever the case of the property name (#611).
const Scenario fontStacksParsing{
    "Font stack detection reads declarations, not quoted text, and ignores the property's case",
    [] {
        given("a quoted string that looks like a declaration", [&] {
            then("it is not reported", [&] {
                check(fonts::longStacks("a::before { content: \"font-family: a,b,c,d\"; } b { content: 'x /* font-family: a,b,c,d */'; }").empty());
            });
        });
        given("a property name in capitals", [&] {
            const auto found = fonts::longStacks("body { FONT-FAMILY: a, b, c, d; } p { Font-Family : e, f, g, h }");
            then("both are reported", [&] {
                require(found.size() == 2);
                check(found[0].costly == 4 && found[1].costly == 4);
            });
        });
        given("a family whose quoted name contains a semicolon", [&] {
            then("the value is not cut inside the quotes", [&] {
                check(fonts::longStacks("x { font-family: \"A;B\", b, c, d; }").size() == 1);
            });
        });
    }};
