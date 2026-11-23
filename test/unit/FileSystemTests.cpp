#include <webfront/system/FileSystem.hpp>

#include <webfront/testing/Bdd.hpp>
#include "Mocks.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace webfront::bdd;

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

const Scenario filesystemMulti{"filesystem::Multi can combine multiple FileSystems", [] {
    given("IndexFS", [&] {
        MockIndexFS indexFS(".");
        when("opening known filenames", [&] {
            then("should return corresponding File objects", [&] {
                require(indexFS.open("index.html").has_value());
                require(indexFS.open("WebFront.js").has_value());
                require(indexFS.open("favicon.ico").has_value());
            });
        });
        when("opening unknown filenames", [&] {
            then("should return nothing", [&] {
                require(!indexFS.open("lib.js").has_value());
                require(!indexFS.open("bootstrap.js").has_value());
                require(!indexFS.open("dummy.file").has_value());
            });
        });
    });

    given("JsLibFS", [&] {
        MockJsLibFS jsFS(".");
        when("opening known filenames", [&] {
            then("should return file objects", [&] {
                require(jsFS.open("lib.js").has_value());
                require(jsFS.open("bootstrap.js").has_value());
            });
        });
        when("opening unknown filenames", [&] {
            then("should return nothing", [&] {
                require(!jsFS.open("index.html").has_value());
                require(!jsFS.open("WebFront.js").has_value());
                require(!jsFS.open("favicon.ico").has_value());
                require(!jsFS.open("dummy.file").has_value());
            });
        });
    });

    given("A Multi filesystem combining IndexFS and JsLibFS", [&] {
        using MockIndexJsFS = fs::Multi<MockIndexFS, MockJsLibFS>;
        MockIndexJsFS indexJsFS(".");

        when("opening known filenames", [&] {
            then("should return file objects", [&] {
                require(indexJsFS.open("lib.js").has_value());
                require(indexJsFS.open("bootstrap.js").has_value());
                require(indexJsFS.open("index.html").has_value());
                require(indexJsFS.open("WebFront.js").has_value());
                require(indexJsFS.open("favicon.ico").has_value());
            });
        });
        when("opening unknown filenames", [&] {
            then("should return nothing", [&] { require(!indexJsFS.open("dummy.file").has_value()); });
        });
    });

    given("A Multi filesystem combining IndexFS, JsLibFS and MixFS with names which can be served by multiple filesystems", [&] {
        using FS = fs::Multi<MockIndexFS, MockJsLibFS, MockMixFS>;
        FS fs("testPath");
        MockIndexFS::openingsCounter = 0;
        MockJsLibFS::openingsCounter = 0;
        MockMixFS::openingsCounter = 0;

        require(MockIndexFS::root == "testPath");
        require(MockJsLibFS::root == "testPath");
        require(MockMixFS::root == "testPath");

        when("opening ambiguous filenames", [&] {
            then("First FS in the parameter list should serve the file", [&] {
                require(fs.open("lib.js").has_value());
                require(MockIndexFS::openingsCounter == 0);
                require(MockJsLibFS::openingsCounter == 1);
                require(MockMixFS::openingsCounter == 0);
                require(fs.open("lib2.js").has_value());
                require(MockIndexFS::openingsCounter == 0);
                require(MockJsLibFS::openingsCounter == 1);
                require(MockMixFS::openingsCounter == 1);
                require(fs.open("bootstrap.js").has_value());
                require(MockIndexFS::openingsCounter == 0);
                require(MockJsLibFS::openingsCounter == 2);
                require(MockMixFS::openingsCounter == 1);
                require(fs.open("index.html").has_value());
                require(MockIndexFS::openingsCounter == 1);
                require(MockJsLibFS::openingsCounter == 2);
                require(MockMixFS::openingsCounter == 1);
                require(fs.open("WebFront.js").has_value());
                require(MockIndexFS::openingsCounter == 2);
                require(MockJsLibFS::openingsCounter == 2);
                require(MockMixFS::openingsCounter == 1);
                require(fs.open("favicon.ico").has_value());
                require(MockIndexFS::openingsCounter == 3);
                require(MockJsLibFS::openingsCounter == 2);
                require(MockMixFS::openingsCounter == 1);
            });
        });
    });
}};

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

const Scenario embeddedFile{"An embedded file reads the same bytes whatever the buffer size", [] {
    for (const size_t bufferSize : array<size_t, 6>{1, 3, 7, 8, 13, 4096}) {
        given("A buffer of " + to_string(bufferSize) + " bytes", [&] {
            then("Embedded data reads back unchanged, across its 64-bit words", [&] {
                require(readAll(fs::File{DigitsAndLetters{}}, bufferSize) == "0123456789abcdefghi");
            });
        });
    }
}};
