#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <system/ToolStore.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;
using namespace webfront;
using webfront::test::TemporaryDirectory;

namespace {

vector<uint8_t> bytes(string_view text) {
    return {text.begin(), text.end()};
}

string contentOf(const filesystem::path& file) {
    ifstream      stream(file, ios::binary);
    ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

fs::ToolRecord plotRecord(string version = "1.2.0") {
    return {.name = "@acme/plot", .version = std::move(version), .integrity = "sha512-abc", .entry = "dist/index.html", .title = "Live plot"};
}

void install(fs::ToolStore& store, const fs::ToolRecord& record, string_view page = "<p>plot</p>") {
    store.begin(record.name, record.version);
    store.write(record.name, record.version, {.path = record.entry, .offset = 0, .bytes = bytes(page)});
    store.commit(record);
}

}  // namespace

SCENARIO("Package names, versions and paths from the browser are validated") {
    GIVEN("valid npm names, versions and package paths") {
        auto name = GENERATE("plot", "webfront-plot", "@acme/plot", "a.b_c~d");
        THEN("they are accepted") {
            REQUIRE_NOTHROW(fs::tools::validatePackageName(name));
            REQUIRE_NOTHROW(fs::tools::validateVersion("1.2.0-beta.1+build.5"));
            REQUIRE(fs::tools::safeRelativePath("dist/assets/app.js") == filesystem::path("dist") / "assets" / "app.js");
        }
    }
    GIVEN("names that could leave the tools directory or are not npm names") {
        auto name = GENERATE("", ".", "..", "../x", "@acme", "@acme/", "@/plot", "@acme/../x", "@acme/a/b", "Plot", "_plot", "a b", "a\\b");
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::tools::validatePackageName(name), invalid_argument);
        }
    }
    GIVEN("names and versions longer than npm accepts") {
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::tools::validatePackageName(string(215, 'a')), invalid_argument);
            REQUIRE_THROWS_AS(fs::tools::validateVersion("1" + string(256, '0')), invalid_argument);
        }
    }
    GIVEN("invalid versions") {
        auto version = GENERATE("", "..", "v1.0.0", "1.0/0", "1.0 0");
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::tools::validateVersion(version), invalid_argument);
        }
    }
    GIVEN("unsafe paths") {
        auto path = GENERATE(as<string>{}, "", "/etc/passwd", "../x", "a/../../x", "a/./b", "a//b", "a/", "..", "C:/x", "c:x", "a\\..\\b", string("a\0b", 3));
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::tools::safeRelativePath(path), invalid_argument);
        }
    }
}

SCENARIO("The lock file is read and written as JSON") {
    GIVEN("two installed tools") {
        fs::tools::LockedTools tools;
        tools.emplace("@acme/plot", plotRecord());
        tools.emplace("logs", fs::ToolRecord{.name = "logs", .version = "0.1.0", .integrity = "", .entry = "index.html", .title = "Say \"hi\"\\\n\x01"});

        WHEN("they are formatted and parsed back") {
            const auto json = fs::tools::formatLock(tools);
            THEN("the same records are read, sorted by name") {
                REQUIRE(json.starts_with("{\n  \"lockfileVersion\": 1,\n  \"tools\": {\n    \"@acme/plot\": {"));
                REQUIRE(fs::tools::parseLock(json) == tools);
            }
        }
    }
    GIVEN("no tool") {
        THEN("an empty lock file round-trips") {
            REQUIRE(fs::tools::parseLock(fs::tools::formatLock({})).empty());
        }
    }
    GIVEN("a lock file edited by hand, with unknown members and escapes") {
        const auto json = R"({"other": [1, true, null, {"a": "b"}], "tools": {"plot": {"version": "1.0.0", "extra": 3,
            "entry": "index.html", "title": "\u00e9\ud83d\ude00\/"}}})";
        THEN("the unknown members are skipped and the escapes decoded") {
            const auto tools = fs::tools::parseLock(json);
            REQUIRE(tools.at("plot").version == "1.0.0");
            REQUIRE(tools.at("plot").title == "\xc3\xa9\xf0\x9f\x98\x80/");
        }
    }
    GIVEN("empty arrays and objects among the unknown members") {
        THEN("they are skipped") {
            REQUIRE(fs::tools::parseLock(R"({"a": [], "b": {}, "c": [[]], "tools": {}})").empty());
        }
    }
    GIVEN("every escape of the JSON grammar") {
        const auto json = R"({"tools": {"plot": {"version": "1.0.0", "entry": "index.html", "title": "\"\\\b\f\n\r\t\u20AC\u0041"}}})";
        THEN("each one is decoded") {
            REQUIRE(fs::tools::parseLock(json).at("plot").title
                    == "\"\\\b\f\n\r\t\xe2\x82\xac"
                       "A");
        }
    }
    GIVEN("malformed JSON") {
        auto json = GENERATE("",
                             "{",
                             R"({"tools": {"plot": {"version": "1.0.0"}})",
                             R"({"tools": 1} x)",
                             R"({"tools": {"p": {"title": "\x"}}})",
                             R"({"tools": {"p": {"title": "\ud800"}}})",
                             R"({"tools": {"p": {"title": "\udc00"}}})",
                             R"({"tools": {"p": {"title": "\ud800\u0041"}}})",
                             R"({"tools": {"p": {"title": "\u12G4"}}})",
                             R"({"tools": {"p": {"title": "\u12"}}})",
                             R"({"other": [1, }})",
                             R"({"other": 1} x)",
                             R"({"other": "abc)",
                             R"({"tools": {"p": {"x": }}})",
                             "{\"tools\": {\"p\": {\"title\": \"a\nb\"}}}");
        THEN("parsing throws") {
            REQUIRE_THROWS(fs::tools::parseLock(json));
        }
    }
    GIVEN("a record pointing outside the tools directory") {
        auto json = GENERATE(R"({"tools": {"../x": {"version": "1.0.0", "entry": "index.html"}}})",
                             R"({"tools": {"plot": {"version": "../1", "entry": "index.html"}}})",
                             R"({"tools": {"plot": {"version": "1.0.0", "entry": "../../secret"}}})");
        THEN("parsing rejects it") {
            REQUIRE_THROWS_AS(fs::tools::parseLock(json), invalid_argument);
        }
    }
}

SCENARIO("A tool store installs tools in its project directory") {
    GIVEN("an empty project") {
        TemporaryDirectory project;
        fs::ToolStore      store{project.path()};
        REQUIRE(store.installed().empty());

        WHEN("a tool is installed in several chunks") {
            store.begin("@acme/plot", "1.2.0");
            store.write("@acme/plot", "1.2.0", {.path = "dist/index.html", .offset = 0, .bytes = bytes("<p>")});
            store.write("@acme/plot", "1.2.0", {.path = "dist/index.html", .offset = 3, .bytes = bytes("plot</p>")});
            store.write("@acme/plot", "1.2.0", {.path = "package.json", .offset = 0, .bytes = bytes("{}")});
            store.commit(plotRecord());

            THEN("its files are in its version directory and the lock file records it") {
                const auto directory = project.path() / ".webfront" / "tools" / "@acme" / "plot@1.2.0";
                REQUIRE(store.installDir("@acme/plot", "1.2.0") == directory);
                REQUIRE(contentOf(directory / "dist" / "index.html") == "<p>plot</p>");
                REQUIRE(contentOf(directory / "package.json") == "{}");
                REQUIRE(store.find("@acme/plot") == plotRecord());
                REQUIRE(fs::ToolStore::readLock(project.path() / "webfront-tools.lock.json").size() == 1);
                REQUIRE_FALSE(filesystem::exists(project.path() / "webfront-tools.lock.json.tmp"));
            }
            AND_WHEN("another version is installed") {
                install(store, plotRecord("2.0.0"), "<p>v2</p>");
                THEN("it replaces the previous one") {
                    REQUIRE(store.find("@acme/plot")->version == "2.0.0");
                    REQUIRE_FALSE(filesystem::exists(store.installDir("@acme/plot", "1.2.0")));
                    REQUIRE(contentOf(store.installDir("@acme/plot", "2.0.0") / "dist" / "index.html") == "<p>v2</p>");
                }
            }
            AND_WHEN("the same version is installed again") {
                install(store, plotRecord(), "<p>again</p>");
                THEN("its files are replaced") {
                    REQUIRE(contentOf(store.installDir("@acme/plot", "1.2.0") / "dist" / "index.html") == "<p>again</p>");
                }
            }
            AND_WHEN("a new installation is abandoned") {
                store.begin("@acme/plot", "3.0.0");
                store.write("@acme/plot", "3.0.0", {.path = "dist/index.html", .offset = 0, .bytes = bytes("<p>v3</p>")});
                THEN("the installed version is untouched") {
                    REQUIRE(store.find("@acme/plot")->version == "1.2.0");
                    REQUIRE(contentOf(store.installDir("@acme/plot", "1.2.0") / "dist" / "index.html") == "<p>plot</p>");
                }
            }
            AND_WHEN("it is removed") {
                REQUIRE(store.remove("@acme/plot"));
                THEN("its files and its lock entry are gone") {
                    REQUIRE_FALSE(filesystem::exists(store.installDir("@acme/plot", "1.2.0")));
                    REQUIRE(store.installed().empty());
                    REQUIRE_FALSE(store.remove("@acme/plot"));
                }
            }
        }
    }
}

SCENARIO("A tool store rejects inconsistent installations") {
    GIVEN("a project with an installation in progress") {
        TemporaryDirectory project;
        fs::ToolStore      store{project.path()};
        store.begin("plot", "1.0.0");
        store.write("plot", "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("abc")});

        THEN("a chunk that does not follow the written bytes is rejected") {
            REQUIRE_THROWS_AS(store.write("plot", "1.0.0", {.path = "index.html", .offset = 2, .bytes = bytes("x")}), invalid_argument);
            REQUIRE_THROWS_AS(store.write("plot", "1.0.0", {.path = "missing.js", .offset = 1, .bytes = bytes("x")}), invalid_argument);
        }
        THEN("a file that cannot be written is reported") {
            store.write("plot", "1.0.0", {.path = "dir/file.js", .offset = 0, .bytes = bytes("x")});
            REQUIRE_THROWS_AS(store.write("plot", "1.0.0", {.path = "dir", .offset = 0, .bytes = bytes("x")}), runtime_error);
        }
        THEN("an unsafe path is rejected") {
            REQUIRE_THROWS_AS(store.write("plot", "1.0.0", {.path = "../escape.js", .offset = 0, .bytes = bytes("x")}), invalid_argument);
            REQUIRE_FALSE(filesystem::exists(project.path() / ".webfront" / "tools" / ".staging" / "escape.js"));
        }
        THEN("writing or committing a version that was not begun is rejected") {
            REQUIRE_THROWS_AS(store.write("plot", "2.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("x")}), invalid_argument);
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "2.0.0", .entry = "index.html"}), invalid_argument);
        }
        THEN("a lock file that cannot be written fails the commit") {
            filesystem::create_directories(project.path() / "webfront-tools.lock.json.tmp");
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"}), runtime_error);
        }
        THEN("a commit whose entry page was not written is rejected") {
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "missing.html"}), invalid_argument);
            REQUIRE(store.installed().empty());
        }
        WHEN("the installation is committed") {
            store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"});
            THEN("it cannot be committed or written again without a new begin") {
                REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"}), invalid_argument);
                REQUIRE_THROWS_AS(store.write("plot", "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("x")}), invalid_argument);
            }
        }
    }
    GIVEN("a malformed lock file") {
        TemporaryDirectory project;
        ofstream(project.path() / "webfront-tools.lock.json") << "{ not json";
        fs::ToolStore store{project.path()};
        THEN("reading the installed tools reports it") {
            REQUIRE_THROWS_AS(store.installed(), runtime_error);
        }
    }
}
