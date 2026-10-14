#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <system/ToolStore.hpp>
#include <utils/Json.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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
        auto name = GENERATE("",
                             ".",
                             "..",
                             "../x",
                             "@acme",
                             "@acme/",
                             "@/plot",
                             "@acme/../x",
                             "@acme/a/b",
                             "Plot",
                             "_plot",
                             "a b",
                             "a\\b",
                             "a..b",
                             "@acme/a..b",
                             "@a..b/c");
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
        auto version = GENERATE("", "..", "v1.0.0", "1.0/0", "1.0 0", "1..0");
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::tools::validateVersion(version), invalid_argument);
        }
    }
    GIVEN("unsafe paths") {
        auto path = GENERATE(as<string>{},
                             "",
                             "/etc/passwd",
                             "../x",
                             "a/../../x",
                             "a/./b",
                             "a//b",
                             "a/",
                             "..",
                             "C:/x",
                             "c:x",
                             "a\\..\\b",
                             "a?.js",
                             "a*.js",
                             "a<b",
                             "a>b",
                             "a|b",
                             "a\"b",
                             "main..html",
                             "a/b..c/d",
                             string("a\0b", 3));
        THEN("they are rejected") {
            REQUIRE_THROWS_AS(fs::tools::safeRelativePath(path), invalid_argument);
        }
    }
}

SCENARIO("The lock file is read and written as JSON") {
    GIVEN("two installed tools") {
        fs::tools::LockedTools tools;
        tools.emplace("@acme/plot", plotRecord());
        tools.emplace("logs", fs::ToolRecord{.name = "logs", .version = "0.1.0", .integrity = "", .entry = "index.html", .title = "Say \"hi\" \\o/"});

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
        json::Reader reader{R"("\"\\\/\b\f\n\r\t\u20AC\u0041")"};
        THEN("each one is decoded") {
            REQUIRE(reader.string()
                    == "\"\\/\b\f\n\r\t\xe2\x82\xac"
                       "A");
        }
    }
}

SCENARIO("The lock file reader is strict") {
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
                             R"({"tools": {}, "other": garbage})",
                             R"({"tools": {}, "lockfileVersion": 1e+})",
                             R"({"a": 01})",
                             R"({"a": -})",
                             R"({"a": 1.})",
                             R"({"a": .5})",
                             R"({"a": +1})",
                             R"({"a": 1e})",
                             R"({"a": tru})",
                             R"({"a": nulls})",
                             "{\"tools\": {\"p\": {\"title\": \"a\nb\"}}}");
        THEN("parsing throws") {
            REQUIRE_THROWS(fs::tools::parseLock(json));
        }
    }
    GIVEN("numbers and literals of the JSON grammar among the unknown members") {
        const auto json = R"({"a": -0.5e+10, "b": 1E5, "c": 0, "d": true, "e": false, "f": null, "g": 12.25, "h": -0, "i": 3e-2, "tools": {}})";
        THEN("they are skipped") {
            REQUIRE(fs::tools::parseLock(json).empty());
        }
    }
    GIVEN("strings with control characters") {
        THEN("quoting escapes them") {
            REQUIRE(json::quote("a\nb\x01") == R"("a\u000ab\u0001")");
        }
    }
    GIVEN("a record pointing outside the tools directory or breaking the listing") {
        auto json = GENERATE(R"({"tools": {"../x": {"version": "1.0.0", "entry": "index.html"}}})",
                             R"({"tools": {"plot": {"version": "../1", "entry": "index.html"}}})",
                             R"({"tools": {"plot": {"version": "1.0.0", "entry": "../../secret"}}})",
                             R"({"tools": {"plot": {"version": "1.0.0", "entry": "index.html", "title": "a\nb"}}})",
                             R"({"tools": {"plot": {"version": "1.0.0", "entry": "index.html", "integrity": "a\tb"}}})");
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
        THEN("a title or integrity longer than 1024 bytes is rejected") {
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html", .title = string(1025, 't')}), invalid_argument);
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .integrity = string(1025, 'i'), .entry = "index.html"}), invalid_argument);
            REQUIRE_NOTHROW(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html", .title = string(1024, 't')}));
        }
        THEN("a title or integrity with control characters is rejected") {
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html", .title = "two\nlines"}), invalid_argument);
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .integrity = "a\tb", .entry = "index.html"}), invalid_argument);
            REQUIRE(store.installed().empty());
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
}

SCENARIO("A tool store refuses lock files it cannot read") {
    GIVEN("a lock file that exists but cannot be read") {
        TemporaryDirectory project;
        const auto         lockFile = project.path() / "webfront-tools.lock.json";
        filesystem::create_directory(lockFile);
        fs::ToolStore store{project.path()};
        THEN("reading the installed tools fails instead of reporting none") {
            REQUIRE_THROWS_AS(store.installed(), runtime_error);
        }
        THEN("an installation fails without replacing the lock file") {
            store.begin("plot", "1.0.0");
            store.write("plot", "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("x")});
            REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"}), runtime_error);
            REQUIRE(filesystem::is_directory(lockFile));
        }
    }

#ifndef _WIN32
    GIVEN("a lock file without read permission") {
        TemporaryDirectory project;
        const auto         lockFile = project.path() / "webfront-tools.lock.json";
        ofstream(lockFile) << R"({"tools": {"logs": {"version": "1.0.0", "entry": "index.html"}}})";
        filesystem::permissions(lockFile, filesystem::perms::none);
        const bool    readable = ifstream(lockFile).is_open();  // e.g. when the tests run as root
        fs::ToolStore store{project.path()};
        THEN("an installation fails instead of dropping the recorded tools") {
            store.begin("plot", "1.0.0");
            store.write("plot", "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("x")});
            if (!readable)
                REQUIRE_THROWS_AS(store.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"}), runtime_error);
            filesystem::permissions(lockFile, filesystem::perms::owner_read | filesystem::perms::owner_write);
            REQUIRE(store.find("logs"));
        }
    }
#endif
    GIVEN("a malformed lock file") {
        TemporaryDirectory project;
        ofstream(project.path() / "webfront-tools.lock.json") << "{ not json";
        fs::ToolStore store{project.path()};
        THEN("reading the installed tools reports it") {
            REQUIRE_THROWS_AS(store.installed(), runtime_error);
        }
    }
}

SCENARIO("A tool store keeps the installed version when a replacement does not complete") {
    GIVEN("a project with an installed tool") {
        TemporaryDirectory project;
        fs::ToolStore      store{project.path()};
        install(store, plotRecord());

        WHEN("a version differing only by case is installed, as on a case-insensitive volume") {
            install(store, plotRecord("1.2.0-alpha"), "<p>alpha</p>");
            install(store, plotRecord("1.2.0-Alpha"), "<p>Alpha</p>");
            THEN("the cleanup of the replaced version keeps the new files") {
                REQUIRE(store.find("@acme/plot")->version == "1.2.0-Alpha");
                REQUIRE(contentOf(store.installDir("@acme/plot", "1.2.0-Alpha") / "dist" / "index.html") == "<p>Alpha</p>");
            }
        }
        WHEN("installing a version fails when the lock file is written") {
            filesystem::create_directories(project.path() / "webfront-tools.lock.json.tmp");
            const auto failingInstall = [&](string version) {
                REQUIRE_THROWS_AS(install(store, plotRecord(std::move(version)), "<p>failed</p>"), runtime_error);
            };
            failingInstall("1.2.0");
            failingInstall("2.0.0");
            THEN("the installed version and its files are intact, and no other version is left") {
                REQUIRE(store.find("@acme/plot") == plotRecord());
                REQUIRE(contentOf(store.installDir("@acme/plot", "1.2.0") / "dist" / "index.html") == "<p>plot</p>");
                REQUIRE_FALSE(filesystem::exists(store.installDir("@acme/plot", "2.0.0")));
            }
        }
        WHEN("a new installation is abandoned") {
            store.begin("@acme/plot", "3.0.0");
            store.write("@acme/plot", "3.0.0", {.path = "dist/index.html", .offset = 0, .bytes = bytes("<p>v3</p>")});
            THEN("the installed version is untouched") {
                REQUIRE(store.find("@acme/plot")->version == "1.2.0");
                REQUIRE(contentOf(store.installDir("@acme/plot", "1.2.0") / "dist" / "index.html") == "<p>plot</p>");
            }
        }
    }
}

SCENARIO("Stores of the same project share their transactions") {
    GIVEN("two stores of one project") {
        TemporaryDirectory project;
        fs::ToolStore      first{project.path()};
        fs::ToolStore      second{project.path() / "."};

        THEN("an installation begun in one can be completed in the other") {
            first.begin("plot", "1.0.0");
            second.write("plot", "1.0.0", {.path = "index.html", .offset = 0, .bytes = bytes("x")});
            second.commit({.name = "plot", .version = "1.0.0", .entry = "index.html"});
            REQUIRE(first.find("plot"));
        }
        WHEN("both install tools at the same time") {
            constexpr int toolsPerStore = 25;
            atomic<int>   failures{0};
            const auto    installMany = [&failures](fs::ToolStore& store, const string& prefix) {
                for (int index = 0; index < toolsPerStore; ++index) {
                    try {
                        install(store, {.name = prefix + to_string(index), .version = "1.0.0", .entry = "index.html"});
                    } catch (const exception&) {
                        ++failures;
                    }
                }
            };
            {
                jthread a{installMany, ref(first), "a"};
                jthread b{installMany, ref(second), "b"};
            }
            THEN("every installation succeeds and is recorded with its files") {
                REQUIRE(failures == 0);
                const auto installed = first.installed();
                REQUIRE(installed.size() == 2 * toolsPerStore);
                for (const auto& [name, record] : installed)
                    REQUIRE(filesystem::is_regular_file(first.installDir(name, record.version) / "index.html"));
            }
        }
    }
}

#ifndef _WIN32
namespace {

// Makes a directory read-only (its entries cannot be deleted) until destroyed.
class ReadOnlyDirectory {
public:
    explicit ReadOnlyDirectory(filesystem::path path) : directory(std::move(path)) {
        filesystem::permissions(directory, filesystem::perms::owner_read | filesystem::perms::owner_exec);
    }
    ReadOnlyDirectory(const ReadOnlyDirectory&)            = delete;
    ReadOnlyDirectory& operator=(const ReadOnlyDirectory&) = delete;
    ~ReadOnlyDirectory() {
        error_code ignored;
        filesystem::permissions(directory, filesystem::perms::owner_all, ignored);
    }

private:
    filesystem::path directory;
};

}  // namespace

SCENARIO("Directories that cannot be deleted do not fail completed operations") {
    GIVEN("an installed version whose files cannot be deleted") {
        TemporaryDirectory project;
        fs::ToolStore      store{project.path()};
        install(store, {.name = "plot", .version = "1.0.0", .entry = "index.html"});
        const auto installed = store.installDir("plot", "1.0.0");
        const auto backup    = project.path() / ".webfront" / "tools" / "plot@1.0.0~backup";

        WHEN("another version replaces it") {
            const ReadOnlyDirectory readOnly{installed};
            REQUIRE_NOTHROW(install(store, {.name = "plot", .version = "2.0.0", .entry = "index.html"}));
            THEN("the new version is installed") {
                REQUIRE(store.find("plot")->version == "2.0.0");
            }
        }
        WHEN("the same version is reinstalled") {
            const ReadOnlyDirectory readOnly{installed};
            REQUIRE_NOTHROW(install(store, {.name = "plot", .version = "1.0.0", .entry = "index.html"}, "<p>again</p>"));
            THEN("the new files are installed") {
                REQUIRE(contentOf(installed / "index.html") == "<p>again</p>");
            }
            // The read-only directory was moved to the backup, which could not be deleted.
            error_code ignored;
            filesystem::permissions(backup, filesystem::perms::owner_all, ignored);
        }
        WHEN("it is removed") {
            const ReadOnlyDirectory readOnly{installed};
            THEN("the removal succeeds") {
                REQUIRE(store.remove("plot"));
                REQUIRE(store.installed().empty());
            }
        }
    }
}
#endif
