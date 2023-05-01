#include <system/FileSystem.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>

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

template<typename KnownFiles>
struct MockFileSystem {
    MockFileSystem(auto){};
    struct Data {
        static constexpr std::array<uint64_t, 1> data{0};
        static constexpr size_t dataSize{0};
        static constexpr std::string_view encoding{"br"};
    };

    static std::optional<webfront::filesystem::File> open(std::filesystem::path file) {
        auto filename = file.relative_path().string();
        if (find(KnownFiles::names.cbegin(), KnownFiles::names.cend(), filename) != KnownFiles::names.cend()) {
            openingsCounter++;
            return webfront::filesystem::File{Data{}};
        }
        return {};
    }

    inline static auto openingsCounter = 0;
};

using MockIndexFS = MockFileSystem<IndexNames>;
using MockJsLibFS = MockFileSystem<JsLibNames>;
using MockMixFS = MockFileSystem<MixNames>;

SCENARIO("filesystem::Multi can combine multiple FileSystems") {
    GIVEN("IndexFS") {
        WHEN("opening known filenames") {
            THEN("should return corresponding File objects") {
                REQUIRE(MockIndexFS::open("index.html").has_value());
                REQUIRE(MockIndexFS::open("WebFront.js").has_value());
                REQUIRE(MockIndexFS::open("favicon.ico").has_value());
            }
        }
        WHEN("opening unknown filenames") {
            THEN("should return nothing") {
                REQUIRE(!MockIndexFS::open("lib.js").has_value());
                REQUIRE(!MockIndexFS::open("bootstrap.js").has_value());
                REQUIRE(!MockIndexFS::open("dummy.file").has_value());
            }
        }
    }

    GIVEN("JsLibFS") {
        WHEN("opening known filenames") {
            THEN("should return file objects") {
                REQUIRE(MockJsLibFS::open("lib.js").has_value());
                REQUIRE(MockJsLibFS::open("bootstrap.js").has_value());
            }
        }
        WHEN("opening unknown filenames") {
            THEN("should return nothing") {
                REQUIRE(!MockJsLibFS::open("index.html").has_value());
                REQUIRE(!MockJsLibFS::open("WebFront.js").has_value());
                REQUIRE(!MockJsLibFS::open("favicon.ico").has_value());
                REQUIRE(!MockJsLibFS::open("dummy.file").has_value());
            }
        }
    }

    GIVEN("A Multi filesystem combining IndexFS and JsLibFS") {
        using MockIndexJsFS = webfront::filesystem::Multi<MockIndexFS, MockJsLibFS>;

        WHEN("opening known filenames") {
            THEN("should return file objects") {
                REQUIRE(MockIndexJsFS::open("lib.js").has_value());
                REQUIRE(MockIndexJsFS::open("bootstrap.js").has_value());
                REQUIRE(MockIndexJsFS::open("index.html").has_value());
                REQUIRE(MockIndexJsFS::open("WebFront.js").has_value());
                REQUIRE(MockIndexJsFS::open("favicon.ico").has_value());
            }
        }
        WHEN("opening unknown filenames") {
            THEN("should return nothing") { REQUIRE(!MockIndexJsFS::open("dummy.file").has_value()); }
        }
    }

    GIVEN("A Multi filesystem combining IndexFS, JsLibFS and MixFS with names which can be served by multiple filesystems") {
        using FS = webfront::filesystem::Multi<MockIndexFS, MockJsLibFS, MockMixFS>;
        MockIndexFS::openingsCounter = 0;
        MockJsLibFS::openingsCounter = 0;
        MockMixFS::openingsCounter = 0;

        WHEN("opening ambiguous filenames") {
            THEN("First FS in the parameter list should serve the file") {
                REQUIRE(FS::open("lib.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 0);
                REQUIRE(MockJsLibFS::openingsCounter == 1);
                REQUIRE(MockMixFS::openingsCounter == 0);
                REQUIRE(FS::open("lib2.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 0);
                REQUIRE(MockJsLibFS::openingsCounter == 1);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(FS::open("bootstrap.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 0);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(FS::open("index.html").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 1);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(FS::open("WebFront.js").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 2);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
                REQUIRE(FS::open("favicon.ico").has_value());
                REQUIRE(MockIndexFS::openingsCounter == 3);
                REQUIRE(MockJsLibFS::openingsCounter == 2);
                REQUIRE(MockMixFS::openingsCounter == 1);
            }
        }
    }
}