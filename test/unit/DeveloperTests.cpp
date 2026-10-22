#include "../fixtures/Pi.hpp"
#include "TemporaryDirectory.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <webfront/DeveloperApp.hpp>
#include <webfront/http/MimeType.hpp>

#include <chrono>
#include <fstream>
#include <limits>

using namespace webfront;
using webfront::test::TemporaryDirectory;

namespace {
struct QuietFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {}

    static void open(std::string_view, std::string_view) {}
};

std::vector<std::uint8_t> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}
std::string read(developer::Store& store, std::string path) {
    auto data = store.read(std::move(path), 0);
    return {data.begin(), data.end()};
}
}  // namespace

SCENARIO("Developer publication preserves the installed files until commit") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    GIVEN("an installed output") {
        auto first = store.begin("generated/PiUI.hpp", false);
        store.write(first, "output", 0, bytes("old"));
        store.commit(first);
        WHEN("the next output is interrupted") {
            auto second = store.begin("generated/PiUI.hpp", false);
            store.write(second, "output", 0, bytes("partial"));
            REQUIRE(read(store, "generated/PiUI.hpp") == "old");
            store.abort(second);
            THEN("the installed output remains intact") {
                REQUIRE(read(store, "generated/PiUI.hpp") == "old");
            }
        }
        WHEN("a complete replacement is committed") {
            auto second = store.begin("generated/PiUI.hpp", false);
            store.write(second, "output", 0, bytes("new"));
            store.commit(second);
            THEN("the new output replaces it") {
                REQUIRE(read(store, "generated/PiUI.hpp") == "new");
            }
        }
    }
}

SCENARIO("Developer writes are bounded and paths stay inside the explicit project") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    GIVEN("a staged preview") {
        const auto id = store.begin(".webfront/developer/preview", true);
        WHEN("the browser sends invalid chunks or paths") {
            THEN("none are published") {
                REQUIRE_THROWS(store.write(id, "../escape", 0, bytes("bad")));
                REQUIRE_THROWS(store.write(id, "index.html", 1, bytes("bad")));
                REQUIRE_THROWS(store.write(id, "big", 0, std::vector<std::uint8_t>(developer::chunkSize + 1)));
                REQUIRE_THROWS(store.begin("../escape", false));
                REQUIRE_THROWS(store.read("../escape", 0));
                REQUIRE_THROWS(store.begin(".webfront/developer/staging/x", true));
                REQUIRE_THROWS(store.begin(".webfront", true));
                REQUIRE_THROWS(store.begin(".webfront/developer", true));
                REQUIRE_THROWS(store.begin(".webfront/developer/preview/assets", true));
            }
        }
        WHEN("a zero-length file and a nested file are sent") {
            store.write(id, "empty", 0, {});
            store.write(id, "assets/a.js", 0, bytes("abc"));
            store.write(id, "assets/a.js", 3, bytes("def"));
            store.commit(id);
            THEN("both are readable after commit") {
                REQUIRE(read(store, ".webfront/developer/preview/empty").empty());
                REQUIRE_THROWS(store.read(".webfront/developer/preview/assets", 0));
                REQUIRE(read(store, ".webfront/developer/preview/assets/a.js") == "abcdef");
            }
        }
    }
    GIVEN("an escaping directory symlink") {
        TemporaryDirectory outside;
        std::error_code    error;
        std::filesystem::create_directory_symlink(outside.path(), project.path() / "escape", error);
        if (!error) {
            THEN("reads and destinations are rejected") {
                REQUIRE_THROWS(store.begin("escape/new.hpp", false));
                REQUIRE_THROWS(store.read("escape/secret", 0));
            }
        }
    }
}

SCENARIO("Developer filesystem serves only published resources and protects the bridge") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    fs::DeveloperFS    filesystem{project.path()};
    GIVEN("published preview assets and private project files") {
        const auto id = store.begin(".webfront/developer/preview", true);
        store.write(id, "index.html", 0, bytes("preview"));
        store.commit(id);
        THEN("the preview and bootstrap are served but project sources are not") {
            REQUIRE(filesystem.open("/webfront/developer/preview/index.html"));
            REQUIRE(filesystem.open("/webfront-bootstrap.html"));
            REQUIRE(filesystem.open("/webfront-bootstrap.mjs"));
            REQUIRE(filesystem.open("/WebFront.js"));
            REQUIRE_FALSE(filesystem.open("/CMakeLists.txt"));
            REQUIRE_FALSE(filesystem.open("/webfront/developer/staging/file"));
            REQUIRE_FALSE(filesystem.open("/webfront/developer/preview/../../secret"));
            REQUIRE(http::MimeType("wasm").toString() == "application/wasm");
        }
    }
}

SCENARIO("The PI demonstration returns exact decimal strings and rejects excessive work") {
    REQUIRE(demo::pi(0) == "3");
    REQUIRE(demo::pi(1) == "3.1");
    REQUIRE(demo::pi(50) == "3.14159265358979323846264338327950288419716939937510");
    REQUIRE(demo::pi(1000).size() == 1002);
    REQUIRE_THROWS_AS(demo::pi(1001), std::invalid_argument);
}

SCENARIO("A downloaded starter never replaces a developer's existing project") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    GIVEN("an existing project directory") {
        std::filesystem::create_directories(project.path() / "ui");
        THEN("new-only publication refuses to stage over it") {
            REQUIRE_THROWS(store.begin("ui", true, false));
            REQUIRE(std::filesystem::is_directory(project.path() / "ui"));
        }
    }
    GIVEN("a new starter staged before its destination exists") {
        auto id = store.begin("new-ui", true, false);
        store.write(id, "main.tsx", 0, bytes("starter"));
        WHEN("a local directory appears before commit") {
            std::filesystem::create_directory(project.path() / "new-ui");
            THEN("commit refuses to replace that directory") {
                REQUIRE_THROWS(store.commit(id));
                REQUIRE_FALSE(std::filesystem::exists(project.path() / "new-ui/main.tsx"));
                store.abort(id);
            }
        }
        WHEN("the destination remains new") {
            store.commit(id);
            THEN("the starter is published") {
                REQUIRE(read(store, "new-ui/main.tsx") == "starter");
            }
        }
    }
}

SCENARIO("Transactions abandoned by a disconnected browser never lock the project") {
    TemporaryDirectory project;
    const auto         stagingRoot = project.path() / ".webfront/developer/staging";
    GIVEN("the maximum number of unfinished transactions") {
        developer::Store         store{project.path()};
        std::vector<std::string> ids;
        for (std::size_t index = 0; index < developer::maxTransactions; ++index)
            ids.push_back(store.begin("generated/" + std::to_string(index), true));
        THEN("active transactions still bound the number of publications") {
            REQUIRE_THROWS(store.begin("generated/extra", true));
        }
    }
    GIVEN("unfinished transactions left by a browser that reloaded") {
        developer::Store         store{project.path(), std::chrono::milliseconds{0}};
        std::vector<std::string> ids;
        for (std::size_t index = 0; index < developer::maxTransactions; ++index)
            ids.push_back(store.begin("generated/" + std::to_string(index), true));
        WHEN("a new publication begins") {
            const auto id = store.begin("generated/new", true);
            THEN("the abandoned ones are discarded with their staging") {
                REQUIRE_FALSE(std::filesystem::exists(stagingRoot / ids.front()));
                REQUIRE_THROWS(store.write(ids.front(), "file", 0, bytes("late")));
                store.write(id, "file", 0, bytes("fresh"));
                store.commit(id);
                REQUIRE(read(store, "generated/new/file") == "fresh");
            }
        }
    }
    GIVEN("staging left by a crashed process") {
        std::filesystem::create_directories(stagingRoot / "1");
        std::filesystem::create_directories(stagingRoot / "2");
        std::filesystem::last_write_time(stagingRoot / "1", std::filesystem::file_time_type::clock::now() - std::chrono::hours{2});
        WHEN("the project is opened again") {
            developer::Store store{project.path()};
            THEN("only the stale staging is removed") {
                REQUIRE_FALSE(std::filesystem::exists(stagingRoot / "1"));
                REQUIRE(std::filesystem::exists(stagingRoot / "2"));
            }
        }
    }
    GIVEN("a backup left by a crashed commit") {
        developer::Store store{project.path()};
        auto             first = store.begin("generated/out.hpp", false);
        store.write(first, "output", 0, bytes("old"));
        store.commit(first);
        const auto second = store.begin("generated/out.hpp", false);
        store.write(second, "output", 0, bytes("new"));
        std::filesystem::create_directories(stagingRoot / (second + "-backup/inner"));
        std::ofstream{stagingRoot / (second + "-backup/inner/file")} << "stale";
        THEN("the replacement still commits") {
            store.commit(second);
            REQUIRE(read(store, "generated/out.hpp") == "new");
        }
    }
}

SCENARIO("The staging area is protected however its path is spelled") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    GIVEN("a symbolic link to the staging area inside the project") {
        std::filesystem::create_directories(project.path() / ".webfront/developer/staging");
        const auto      victim = store.begin("victim", true);
        std::error_code error;
        std::filesystem::create_directory_symlink(project.path() / ".webfront/developer/staging", project.path() / "alias", error);
        if (!error) {
            THEN("it cannot be used as a publication destination") {
                REQUIRE_THROWS(store.begin("alias", true));
                REQUIRE_THROWS(store.begin("alias/" + victim, true));
            }
        }
        store.abort(victim);
    }
}

SCENARIO("The developer bridge refuses WebSocket clients without an Origin") {
    GIVEN("the default application configuration") {
        REQUIRE(WebFrontConfig{}.contentPolicy.allowMissingOrigin);
        WHEN("it is prepared for a developer project") {
            const auto config = developer::projectConfig({}, "project");
            THEN("the project is the document root and a missing Origin is refused") {
                REQUIRE(config.documentRoot == "project");
                REQUIRE_FALSE(config.contentPolicy.allowMissingOrigin);
            }
        }
    }
}

SCENARIO("Startup cleanup never leaves the project through a staging link") {
    TemporaryDirectory project;
    TemporaryDirectory outside;
    GIVEN("a staging directory that is a link to an old file outside the project") {
        const auto victim = outside.path() / "keep.txt";
        std::ofstream{victim} << "outside";
        std::filesystem::last_write_time(victim, std::filesystem::file_time_type::clock::now() - std::chrono::hours{3});
        std::filesystem::create_directories(project.path() / ".webfront/developer");
        std::error_code error;
        std::filesystem::create_directory_symlink(outside.path(), project.path() / ".webfront/developer/staging", error);
        if (!error) {
            WHEN("the project is opened") {
                developer::Store store{project.path()};
                THEN("the outside file is untouched") {
                    REQUIRE(std::filesystem::exists(victim));
                }
            }
        }
    }
}

SCENARIO("Paths differing only by case never share a staged file") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    GIVEN("a staged directory holding LICENSE") {
        const auto id = store.begin("package", true);
        store.write(id, "LICENSE", 0, bytes("first"));
        WHEN("a second file whose name differs only by case is sent") {
            THEN("it is refused and the first file keeps its content") {
                REQUIRE_THROWS_AS(store.write(id, "License", 0, bytes("second")), std::invalid_argument);
                store.write(id, "LICENSE", 5, bytes("!"));
                store.commit(id);
                REQUIRE(read(store, "package/LICENSE") == "first!");
            }
        }
    }
}

SCENARIO("The developer store refuses malformed requests without changing the project") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    GIVEN("a project file and a single-output transaction") {
        std::ofstream(project.path() / "notes.txt") << "text";
        const auto id = store.begin("generated/output.hpp", false);
        THEN("invalid reads and writes are rejected") {
            REQUIRE_THROWS_AS(store.read("notes.txt", std::numeric_limits<std::uint64_t>::max()), std::invalid_argument);
            REQUIRE_THROWS_AS(store.write(id, "other", 0, bytes("x")), std::invalid_argument);
            REQUIRE_THROWS_AS(store.commit(id), std::runtime_error);
        }
        THEN("aborting is idempotent and ends the transaction") {
            REQUIRE_NOTHROW(store.abort("unknown"));
            REQUIRE_NOTHROW(store.abort(id));
            REQUIRE_THROWS_AS(store.commit(id), std::runtime_error);
        }
    }
    GIVEN("an existing destination") {
        std::ofstream(project.path() / "taken.txt") << "text";
        THEN("only the replacing form may publish over it") {
            REQUIRE_THROWS_AS(store.beginNew("taken.txt", false), std::runtime_error);
            REQUIRE_NOTHROW(store.abort(store.beginReplacing("taken.txt", false)));
        }
    }
    GIVEN("a path that is not a directory") {
        std::ofstream(project.path() / "file") << "x";
        THEN("it cannot be a project") {
            REQUIRE_THROWS_AS(developer::Store{project.path() / "file"}, std::invalid_argument);
        }
    }
}

SCENARIO("The developer application needs an explicit project and a trusted environment") {
    using App = BasicDeveloperApp<QuietFrontend>;
    const DeveloperBootstrapOptions trusted{.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"};
    GIVEN("no project directory") {
        App app({}, WebFrontConfig{.port = "0"});
        THEN("bootstrap is refused") {
            REQUIRE_THROWS_AS(app.bootstrap("developer", trusted), std::invalid_argument);
        }
    }
    GIVEN("a project directory") {
        TemporaryDirectory project;
        App                app(project.path(), WebFrontConfig{.port = "0"});
        THEN("an unknown mode or an unpinned environment is refused") {
            REQUIRE_THROWS_AS(app.bootstrap("other", trusted), std::invalid_argument);
            REQUIRE_THROWS_AS(app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "md5-abc"}), std::invalid_argument);
            REQUIRE_THROWS_AS(app.bootstrap("developer", {.environment = "", .integrity = "sha512-abc"}), std::invalid_argument);
        }
        THEN("a pinned environment is accepted once") {
            REQUIRE_NOTHROW(app.bootstrap("developer", trusted));
            REQUIRE_THROWS_AS(app.bootstrap("developer", trusted), std::logic_error);
        }
    }
}

namespace {
constexpr auto manifestPath = "webfront.project.json";
constexpr auto lockPath     = "webfront-packages.lock.json";
const std::vector<std::string> pair{manifestPath, lockPath};

void publishFile(developer::Store& store, const std::string& path, std::string_view text) {
    const auto id = store.begin(path, false);
    store.write(id, "output", 0, bytes(text));
    store.commit(id);
}

// Stages new contents for the manifest and the lock, then publishes them together.
void publishPair(developer::Store& store, std::string_view manifest, std::string_view lock, const std::string& expected) {
    const auto first  = store.begin(manifestPath, false);
    const auto second = store.begin(lockPath, false);
    store.write(first, "output", 0, bytes(manifest));
    store.write(second, "output", 0, bytes(lock));
    store.commitGroup({first, second}, pair, expected);
}

[[nodiscard]] std::string projectState(developer::Store& store) {
    return read(store, manifestPath) + "|" + read(store, lockPath);
}

// Stops the process at one step of a group publication, then reopens the project.
[[nodiscard]] std::string crashAt(const std::string& step, const std::filesystem::path& project) {
    {
        developer::Store store{project, developer::defaultAbandonedAfter, [&step](std::string_view name) {
                                   if (name == step)
                                       throw developer::InjectedFault{"crash at " + step};
                               }};
        const auto       expected = store.state(pair);
        REQUIRE_THROWS_AS(publishPair(store, "manifest-2", "lock-2", expected), developer::InjectedFault);
    }
    developer::Store reopened{project};
    return projectState(reopened);
}
}  // namespace

SCENARIO("A manifest and its lock are published together or not at all") {
    TemporaryDirectory project;
    {
        developer::Store store{project.path()};
        publishFile(store, manifestPath, "manifest-1");
        publishFile(store, lockPath, "lock-1");
    }
    GIVEN("an installed manifest and lock") {
        developer::Store store{project.path()};
        WHEN("both files are published after an uninterrupted install") {
            publishPair(store, "manifest-2", "lock-2", store.state(pair));
            THEN("both are replaced and no journal remains") {
                REQUIRE(projectState(store) == "manifest-2|lock-2");
                REQUIRE(std::filesystem::is_empty(project.path() / ".webfront/developer/journal"));
            }
        }
        WHEN("the process stops before the journal is committed") {
            store.state(pair);
            THEN("the previous state is intact after restart") {
                REQUIRE(crashAt("before-journal", project.path()) == "manifest-1|lock-1");
            }
        }
        WHEN("the process stops right after the journal is committed") {
            THEN("restart completes the publication") {
                REQUIRE(crashAt("after-journal", project.path()) == "manifest-2|lock-2");
            }
        }
        WHEN("the process stops between the two replacements") {
            THEN("restart completes the publication") {
                REQUIRE(crashAt("replacement-1", project.path()) == "manifest-2|lock-2");
            }
        }
        WHEN("the process stops after the second replacement") {
            THEN("restart completes the publication") {
                REQUIRE(crashAt("replacement-2", project.path()) == "manifest-2|lock-2");
            }
        }
    }
    GIVEN("a publication committed but interrupted between its replacements") {
        {
            developer::Store store{project.path(), developer::defaultAbandonedAfter, [](std::string_view name) {
                                       if (name == "replacement-1")
                                           throw developer::InjectedFault{"crash"};
                                   }};
            REQUIRE_THROWS_AS(publishPair(store, "manifest-2", "lock-2", store.state(pair)), developer::InjectedFault);
        }
        WHEN("the recovery itself is interrupted, once at each of its steps") {
            for (const char* step : {"replacement-1", "replacement-2"}) {
                REQUIRE_THROWS_AS(developer::Store(project.path(), developer::defaultAbandonedAfter,
                                                   [step](std::string_view name) {
                                                       if (name == step)
                                                           throw developer::InjectedFault{"crash"};
                                                   }),
                                  developer::InjectedFault);
            }
            THEN("a later restart still reaches the new state") {
                developer::Store store{project.path()};
                REQUIRE(projectState(store) == "manifest-2|lock-2");
                REQUIRE(std::filesystem::is_empty(project.path() / ".webfront/developer/journal"));
            }
        }
    }
    GIVEN("leftovers of a publication that never reached its commit point") {
        const auto journal = project.path() / ".webfront/developer/journal";
        std::filesystem::create_directories(journal / "7.d");
        std::ofstream{journal / "7.d" / "0"} << "orphan";
        std::ofstream{journal / "7.tmp"} << "webfront-journal 1\nwebfront.project.json\n0\n";
        std::ofstream{journal / "8.journal"} << "webfront-journal 1\nwebfront.project.json\n0\n";
        WHEN("the project is opened") {
            developer::Store store{project.path()};
            THEN("they are abandoned and the project keeps its files") {
                REQUIRE(std::filesystem::is_empty(journal));
                REQUIRE(projectState(store) == "manifest-1|lock-1");
            }
        }
    }
}

SCENARIO("A publication prepared from stale state is refused") {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    publishFile(store, manifestPath, "manifest-1");
    publishFile(store, lockPath, "lock-1");
    GIVEN("two browsers reading the same state") {
        const auto firstBrowser  = store.state(pair);
        const auto secondBrowser = store.state(pair);
        REQUIRE(firstBrowser == secondBrowser);
        WHEN("the first publishes, then the second does with what it read") {
            publishPair(store, "manifest-a", "lock-a", firstBrowser);
            THEN("the second is refused and nothing is lost") {
                REQUIRE_THROWS_WITH(publishPair(store, "manifest-b", "lock-b", secondBrowser), Catch::Matchers::ContainsSubstring("changed since it was read"));
                REQUIRE(projectState(store) == "manifest-a|lock-a");
            }
            AND_THEN("it succeeds after reading the new state again") {
                publishPair(store, "manifest-ab", "lock-ab", store.state(pair));
                REQUIRE(projectState(store) == "manifest-ab|lock-ab");
            }
        }
        WHEN("a file changes outside the store") {
            publishFile(store, lockPath, "lock-c");
            THEN("the state differs and the group is refused") {
                REQUIRE(store.state(pair) != firstBrowser);
                REQUIRE_THROWS(publishPair(store, "manifest-b", "lock-b", firstBrowser));
            }
        }
    }
    GIVEN("a missing lock") {
        std::filesystem::remove(project.path() / lockPath);
        THEN("its absence is part of the state") {
            const auto absent = store.state(pair);
            publishFile(store, lockPath, "");
            REQUIRE(store.state(pair) != absent);
        }
    }
    GIVEN("malformed groups") {
        const auto first = store.begin(manifestPath, false);
        store.write(first, "output", 0, bytes("x"));
        THEN("they are refused without changing the project") {
            REQUIRE_THROWS(store.commitGroup({}, pair, store.state(pair)));
            REQUIRE_THROWS(store.commitGroup({first, first}, pair, store.state(pair)));
            REQUIRE_THROWS(store.commitGroup({first}, {lockPath}, store.state({lockPath})));
            REQUIRE(projectState(store) == "manifest-1|lock-1");
            store.abort(first);
        }
    }
}
