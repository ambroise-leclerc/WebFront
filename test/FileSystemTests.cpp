#include <system/FileSystem.hpp>

#include <catch2/catch_test_macros.hpp>
#include "Mocks.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace std;
using namespace webfront;

struct IndexNames {
    static inline list names{"index.html", "WebFront.js", "favicon.ico"};
};
struct JsLibNames {
    static inline list names{"lib.js", "bootstrap.js"};
};
struct MixNames {
    static inline list names{"index.html", "WebFront.js", "lib.js", "lib2.js"};
};

using MockIndexFS = MockFileSystem<IndexNames>;
using MockJsLibFS = MockFileSystem<JsLibNames>;
using MockMixFS = MockFileSystem<MixNames>;

SCENARIO("filesystem::Multi can combine multiple FileSystems") {
    GIVEN("IndexFS") {
        MockIndexFS indexFS(".");
        WHEN("opening known filenames") {
            THEN("should return corresponding File objects") {
                REQUIRE(indexFS.open("index.html").has_value());
                REQUIRE(indexFS.open("WebFront.js").has_value());
                REQUIRE(indexFS.open("favicon.ico").has_value());
            }
        }
        WHEN("opening unknown filenames") {
            THEN("should return nothing") {
                REQUIRE(!indexFS.open("lib.js").has_value());
                REQUIRE(!indexFS.open("bootstrap.js").has_value());
                REQUIRE(!indexFS.open("dummy.file").has_value());
            }
        }
    }

    GIVEN("JsLibFS") {
        MockJsLibFS jsFS(".");
        WHEN("opening known filenames") {
            THEN("should return file objects") {
                REQUIRE(jsFS.open("lib.js").has_value());
                REQUIRE(jsFS.open("bootstrap.js").has_value());
            }
        }
        WHEN("opening unknown filenames") {
            THEN("should return nothing") {
                REQUIRE(!jsFS.open("index.html").has_value());
                REQUIRE(!jsFS.open("WebFront.js").has_value());
                REQUIRE(!jsFS.open("favicon.ico").has_value());
                REQUIRE(!jsFS.open("dummy.file").has_value());
            }
        }
    }

    GIVEN("A Multi filesystem combining IndexFS and JsLibFS") {
        using MockIndexJsFS = fs::Multi<MockIndexFS, MockJsLibFS>;
        MockIndexJsFS indexJsFS(".");

        WHEN("opening known filenames") {
            THEN("should return file objects") {
                REQUIRE(indexJsFS.open("lib.js").has_value());
                REQUIRE(indexJsFS.open("bootstrap.js").has_value());
                REQUIRE(indexJsFS.open("index.html").has_value());
                REQUIRE(indexJsFS.open("WebFront.js").has_value());
                REQUIRE(indexJsFS.open("favicon.ico").has_value());
            }
        }
        WHEN("opening unknown filenames") {
            THEN("should return nothing") { REQUIRE(!indexJsFS.open("dummy.file").has_value()); }
        }
    }

    GIVEN("A Multi filesystem combining IndexFS, JsLibFS and MixFS with names which can be served by multiple filesystems") {
        using FS = fs::Multi<MockIndexFS, MockJsLibFS, MockMixFS>;
        FS fs("testPath");
        MockIndexFS::openingsCounter = 0;
        MockJsLibFS::openingsCounter = 0;
        MockMixFS::openingsCounter = 0;

        REQUIRE(MockIndexFS::root == "testPath");
        REQUIRE(MockJsLibFS::root == "testPath");
        REQUIRE(MockMixFS::root == "testPath");

        WHEN("opening ambiguous filenames") {
            THEN("First FS in the parameter list should serve the file") {
                REQUIRE(fs.open("lib.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 0);
                REQUIRE(MockJsLibFS::openingsCounter == 1);
                REQUIRE(MockMixFS::openingsCounter == 0);
                REQUIRE(fs.open("lib2.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 0);
                REQUIRE(MockJsLibFS::openingsCounter == 1);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(fs.open("bootstrap.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 0);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(fs.open("index.html").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 1);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(fs.open("WebFront.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 2);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(fs.open("favicon.ico").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 3);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
            }
        }
    }
}

// "0123456789abcdefghi" packed like the generated asset headers: big-endian 64-bit words, the last
// one padded with zeros.
struct DigitsAndLetters {
    static constexpr size_t             dataSize{19};
    static constexpr array<uint64_t, 3> data{0x3031323334353637, 0x3839616263646566, 0x6768690000000000};
};

static string readAll(fs::File file, size_t bufferSize) {
    string       content;
    vector<char> buffer(bufferSize);
    while (const auto count = file.read(buffer))
        content.append(buffer.data(), count);
    return content;
}

SCENARIO("An embedded file reads the same bytes whatever the buffer size") {
    for (const size_t bufferSize : array<size_t, 6>{1, 3, 7, 8, 13, 4096}) {
        GIVEN("A buffer of " + to_string(bufferSize) + " bytes") {
            THEN("Embedded data reads back unchanged, across its 64-bit words") {
                REQUIRE(readAll(fs::File{DigitsAndLetters{}}, bufferSize) == "0123456789abcdefghi");
            }
        }
    }
}
