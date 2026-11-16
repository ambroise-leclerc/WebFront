#include "TemporaryDirectory.hpp"
#include "Bdd.hpp"
#include <webfront/DeveloperApp.hpp>
#include <webfront/http/MimeType.hpp>

#include <array>
#include <cstring>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <regex>
#include <span>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

using namespace webfront::bdd;

using namespace webfront;
using webfront::test::TemporaryDirectory;

namespace {
struct QuietFrontend {
    static constexpr frontend::Action action{frontend::Action::keepServerRunning};

    static void initialize() {}

    static void open(std::string_view, std::string_view) {}
};

struct ClosingQuietFrontend {
    static constexpr frontend::Action action{frontend::Action::closeServerAfterOpen};

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

const Scenario developerPublication{"Developer publication preserves the installed files until commit", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    given("an installed output", [&] {
        auto first = store.begin("generated/LifeUI.hpp", false);
        store.write(first, "output", 0, bytes("old"));
        store.commit(first);
        when("the next output is interrupted", [&] {
            auto second = store.begin("generated/LifeUI.hpp", false);
            store.write(second, "output", 0, bytes("partial"));
            require(read(store, "generated/LifeUI.hpp") == "old");
            store.abort(second);
            then("the installed output remains intact", [&] {
                require(read(store, "generated/LifeUI.hpp") == "old");
            });
        });
        when("a complete replacement is committed", [&] {
            auto second = store.begin("generated/LifeUI.hpp", false);
            store.write(second, "output", 0, bytes("new"));
            store.commit(second);
            then("the new output replaces it", [&] {
                require(read(store, "generated/LifeUI.hpp") == "new");
            });
        });
    });
}};

const Scenario publicationWaits{"A publication waits for a file that another reader is about to release", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    given("a published directory with a file held open, as a scan of what was just written holds it on Windows", [&] {
        auto first = store.begin("packages/library", true);
        store.write(first, "index.js", 0, bytes("old"));
        store.commit(first);
        std::ifstream held(project.path() / "packages/library/index.js", std::ios::binary);
        require(held.is_open());
        auto released = std::async(std::launch::async, [&held] {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            held.close();
        });
        when("the directory is replaced before the reader closes the file", [&] {
            auto second = store.begin("packages/library", true);
            store.write(second, "index.js", 0, bytes("new"));
            store.commit(second);
            then("the publication is completed once the file is released", [&] {
                require(read(store, "packages/library/index.js") == "new");
            });
        });
        released.get();
    });
}};

const Scenario publishedHTTP{"A published HTTP source remains readable across directory replacement", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    fs::DeveloperFS    filesystem{project.path()};
    std::string        previous = "old-body";
    while (previous.size() < 200000)
        previous += static_cast<char>('a' + previous.size() % 26);
    auto first = store.begin(".webfront/developer/preview", true);
    store.write(first, "app.js", 0, bytes(previous));
    store.commit(first);
    auto held = filesystem.open("/webfront/developer/preview/app.js");
    require(held);
    require(held->knownSize() == previous.size());
    auto second = store.begin(".webfront/developer/preview", true);
    store.write(second, "app.js", 0, bytes("new-body"));
    requireNothrow([&] { store.commit(second); });
    std::array<char, 8> content{};
    require(held->read(content) == content.size());
    require(std::string_view(content.data(), content.size()) == "old-body");
    require(held->seek(4));
    require(held->read(std::span(content).first(4)) == 4);
    require(std::string_view(content.data(), 4) == "body");
    require(held->seek(65531));
    require(held->read(content) == content.size());
    require(std::string_view(content.data(), content.size()) == std::string_view(previous).substr(65531, content.size()));
    require(held->seek(previous.size() - 3));
    require(held->read(content) == 3);
    require(std::string_view(content.data(), 3) == std::string_view(previous).substr(previous.size() - 3));
    require(held->read(content) == 0);
    auto current = filesystem.open("/webfront/developer/preview/app.js");
    require(current);
    require(current->read(content) == content.size());
    require(std::string_view(content.data(), content.size()) == "new-body");
    auto third = store.begin(".webfront/developer/preview", true);
    store.write(third, "app.js", 0, {});
    requireNothrow([&] { store.commit(third); });
    auto empty = filesystem.open("/webfront/developer/preview/app.js");
    require(empty);
    require(empty->knownSize() == 0);
    require(empty->read(content) == 0);
    require(current->seek(0));
    require(current->read(content) == content.size());
    require(std::string_view(content.data(), content.size()) == "new-body");
}};

const Scenario developerWrites{"Developer writes are bounded and paths stay inside the explicit project", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    given("a staged preview", [&] {
        const auto id = store.begin(".webfront/developer/preview", true);
        when("the browser sends invalid chunks or paths", [&] {
            then("none are published", [&] {
                requireThrows([&] { store.write(id, "../escape", 0, bytes("bad")); });
                requireThrows([&] { store.write(id, "index.html", 1, bytes("bad")); });
                requireThrows([&] { store.write(id, "big", 0, std::vector<std::uint8_t>(developer::chunkSize + 1)); });
                requireThrows([&] { store.begin("../escape", false); });
                requireThrows([&] { store.read("../escape", 0); });
                requireThrows([&] { store.begin(".webfront/developer/staging/x", true); });
                requireThrows([&] { store.begin(".webfront", true); });
                requireThrows([&] { store.begin(".webfront/developer", true); });
                requireThrows([&] { store.begin(".webfront/developer/preview/assets", true); });
            });
        });
        when("a zero-length file and a nested file are sent", [&] {
            store.write(id, "empty", 0, {});
            store.write(id, "assets/a.js", 0, bytes("abc"));
            store.write(id, "assets/a.js", 3, bytes("def"));
            store.commit(id);
            then("both are readable after commit", [&] {
                require(read(store, ".webfront/developer/preview/empty").empty());
                requireThrows([&] { store.read(".webfront/developer/preview/assets", 0); });
                require(read(store, ".webfront/developer/preview/assets/a.js") == "abcdef");
            });
        });
    });
    given("an escaping directory symlink", [&] {
        TemporaryDirectory outside;
        std::error_code    error;
        std::filesystem::create_directory_symlink(outside.path(), project.path() / "escape", error);
        if (!error) {
            then("reads and destinations are rejected", [&] {
                requireThrows([&] { store.begin("escape/new.hpp", false); });
                requireThrows([&] { store.read("escape/secret", 0); });
            });
        }
    });
}};

const Scenario developerFilesystem{"Developer filesystem serves only published resources and protects the bridge", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    fs::DeveloperFS    filesystem{project.path()};
    given("published preview and test runner assets and private project files", [&] {
        const auto id = store.begin(".webfront/developer/preview", true);
        store.write(id, "index.html", 0, bytes("preview"));
        store.commit(id);
        const auto runner = store.begin(".webfront/developer/test", true);
        store.write(runner, "index.html", 0, bytes("runner"));
        store.commit(runner);
        then("the preview and bootstrap are served but project sources are not", [&] {
            require(filesystem.open("/webfront/developer/preview/index.html"));
            require(filesystem.open("/webfront/developer/test/index.html"));
            require(!filesystem.open("/webfront/developer/build/app.js"));
            require(filesystem.open("/webfront-bootstrap.html"));
            require(filesystem.open("/webfront-bootstrap.mjs"));
            require(filesystem.open("/WebFront.js"));
            require(!filesystem.open("/CMakeLists.txt"));
            require(!filesystem.open("/webfront/developer/staging/file"));
            require(!filesystem.open("/webfront/developer/preview/../../secret"));
            require(http::MimeType("wasm").toString() == "application/wasm");
        });
    });
}};

const Scenario downloadedStarter{"A downloaded starter never replaces a developer's existing project", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    given("an existing project directory", [&] {
        std::filesystem::create_directories(project.path() / "ui");
        then("new-only publication refuses to stage over it", [&] {
            requireThrows([&] { store.begin("ui", true, false); });
            require(std::filesystem::is_directory(project.path() / "ui"));
        });
    });
    given("a new starter staged before its destination exists", [&] {
        auto id = store.begin("new-ui", true, false);
        store.write(id, "main.tsx", 0, bytes("starter"));
        when("a local directory appears before commit", [&] {
            std::filesystem::create_directory(project.path() / "new-ui");
            then("commit refuses to replace that directory", [&] {
                requireThrows([&] { store.commit(id); });
                require(!std::filesystem::exists(project.path() / "new-ui/main.tsx"));
                store.abort(id);
            });
        });
        when("the destination remains new", [&] {
            store.commit(id);
            then("the starter is published", [&] {
                require(read(store, "new-ui/main.tsx") == "starter");
            });
        });
    });
}};

const Scenario transactionsAbandoned{"Transactions abandoned by a disconnected browser never lock the project", [] {
    TemporaryDirectory project;
    const auto         stagingRoot = project.path() / ".webfront/developer/staging";
    given("the maximum number of unfinished transactions", [&] {
        developer::Store         store{project.path()};
        std::vector<std::string> ids;
        for (std::size_t index = 0; index < developer::maxTransactions; ++index)
            ids.push_back(store.begin("generated/" + std::to_string(index), true));
        then("active transactions still bound the number of publications", [&] {
            requireThrows([&] { store.begin("generated/extra", true); });
        });
    });
    given("unfinished transactions left by a browser that reloaded", [&] {
        developer::Store         store{project.path(), std::chrono::milliseconds{0}};
        std::vector<std::string> ids;
        for (std::size_t index = 0; index < developer::maxTransactions; ++index)
            ids.push_back(store.begin("generated/" + std::to_string(index), true));
        when("a new publication begins", [&] {
            const auto id = store.begin("generated/new", true);
            then("the abandoned ones are discarded with their staging", [&] {
                require(!std::filesystem::exists(stagingRoot / ids.front()));
                requireThrows([&] { store.write(ids.front(), "file", 0, bytes("late")); });
                store.write(id, "file", 0, bytes("fresh"));
                store.commit(id);
                require(read(store, "generated/new/file") == "fresh");
            });
        });
    });
    given("staging left by a crashed process", [&] {
        std::filesystem::create_directories(stagingRoot / "1");
        std::filesystem::create_directories(stagingRoot / "2");
        std::filesystem::last_write_time(stagingRoot / "1", std::filesystem::file_time_type::clock::now() - std::chrono::hours{2});
        when("the project is opened again", [&] {
            developer::Store store{project.path()};
            then("only the stale staging is removed", [&] {
                require(!std::filesystem::exists(stagingRoot / "1"));
                require(std::filesystem::exists(stagingRoot / "2"));
            });
        });
    });
    given("a backup left by a crashed commit", [&] {
        developer::Store store{project.path()};
        auto             first = store.begin("generated/out.hpp", false);
        store.write(first, "output", 0, bytes("old"));
        store.commit(first);
        const auto second = store.begin("generated/out.hpp", false);
        store.write(second, "output", 0, bytes("new"));
        std::filesystem::create_directories(stagingRoot / (second + "-backup/inner"));
        std::ofstream{stagingRoot / (second + "-backup/inner/file")} << "stale";
        then("the replacement still commits", [&] {
            store.commit(second);
            require(read(store, "generated/out.hpp") == "new");
        });
    });
}};

const Scenario linkNever{"A link never turns the staging area into a destination or leads its cleanup outside the project", [] {
    TemporaryDirectory project;
    given("a symbolic link to the staging area inside the project", [&] {
        developer::Store store{project.path()};
        std::filesystem::create_directories(project.path() / ".webfront/developer/staging");
        const auto      victim = store.begin("victim", true);
        std::error_code error;
        std::filesystem::create_directory_symlink(project.path() / ".webfront/developer/staging", project.path() / "alias", error);
        if (!error) {
            then("it cannot be used as a publication destination", [&] {
                requireThrows([&] { store.begin("alias", true); });
                requireThrows([&] { store.begin("alias/" + victim, true); });
            });
        }
        store.abort(victim);
    });
    given("a staging directory that is a link to an old file outside the project", [&] {
        TemporaryDirectory outside;
        const auto         victim = outside.path() / "keep.txt";
        std::ofstream{victim} << "outside";
        std::filesystem::last_write_time(victim, std::filesystem::file_time_type::clock::now() - std::chrono::hours{3});
        std::filesystem::create_directories(project.path() / ".webfront/developer");
        std::error_code error;
        std::filesystem::create_directory_symlink(outside.path(), project.path() / ".webfront/developer/staging", error);
        if (!error) {
            when("the project is opened, which cleans the stale staging", [&] {
                developer::Store store{project.path()};
                then("the outside file is untouched", [&] {
                    require(std::filesystem::exists(victim));
                });
            });
        }
    });
}};

const Scenario developerBridge{"The developer bridge refuses WebSocket clients without an Origin", [] {
    given("the default application configuration", [&] {
        require(WebFrontConfig{}.contentPolicy.allowMissingOrigin);
        when("it is prepared for a developer project", [&] {
            const auto config = developer::projectConfig({}, "project");
            then("the project is the document root and a missing Origin is refused", [&] {
                require(config.documentRoot == "project");
                require(!config.contentPolicy.allowMissingOrigin);
            });
        });
    });
}};

const Scenario developerStore{"The developer store refuses malformed requests without changing the project", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    given("a staged directory holding LICENSE", [&] {
        const auto id = store.begin("package", true);
        store.write(id, "LICENSE", 0, bytes("first"));
        then("a second file whose name differs only by case is refused, and the first keeps its content", [&] {
            requireThrows<std::invalid_argument>([&] { store.write(id, "License", 0, bytes("second")); });
            store.write(id, "LICENSE", 5, bytes("!"));
            store.commit(id);
            require(read(store, "package/LICENSE") == "first!");
        });
    });
    given("a project file and a single-output transaction", [&] {
        std::ofstream(project.path() / "notes.txt") << "text";
        const auto id = store.begin("generated/output.hpp", false);
        then("invalid reads and writes are rejected", [&] {
            requireThrows<std::invalid_argument>([&] { store.read("notes.txt", std::numeric_limits<std::uint64_t>::max()); });
            requireThrows<std::invalid_argument>([&] { store.write(id, "other", 0, bytes("x")); });
            requireThrows<std::runtime_error>([&] { store.commit(id); });
        });
        then("aborting is idempotent and ends the transaction", [&] {
            requireNothrow([&] { store.abort("unknown"); });
            requireNothrow([&] { store.abort(id); });
            requireThrows<std::runtime_error>([&] { store.commit(id); });
        });
    });
    given("an existing destination", [&] {
        std::ofstream(project.path() / "taken.txt") << "text";
        then("only the replacing form may publish over it", [&] {
            requireThrows<std::runtime_error>([&] { store.beginNew("taken.txt", false); });
            requireNothrow([&] { store.abort(store.beginReplacing("taken.txt", false)); });
        });
    });
    given("a path that is not a directory", [&] {
        std::ofstream(project.path() / "file") << "x";
        then("it cannot be a project", [&] {
            requireThrows<std::invalid_argument>([&] { developer::Store{project.path() / "file"}; });
        });
    });
}};

const Scenario developerApplication{"A developer application is not told to use the developer environment", [] {
    using App = BasicDeveloperApp<ClosingQuietFrontend>;
    TemporaryDirectory project;
    std::ostringstream captured;
    struct Redirect {
        explicit Redirect(std::ostream& out) {
            detail::developmentPathOutput = &out;
            detail::developmentPathNoted.clear();
        }
        ~Redirect() { detail::developmentPathOutput = nullptr; }
    } const redirect{captured};
    given("a project whose developer environment is bootstrapped", [&] {
        App app(project.path(), WebFrontConfig{.port = "0"});
        app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"});
        when("it is shown", [&] {
            app.show();
            then("its document root, the project that wf builds, raises no note", [&] {
                require(captured.str().find(detail::developmentPathNote) == std::string::npos);
            });
        });
    });
    given("the same application without its developer environment", [&] {
        App app(project.path(), WebFrontConfig{.port = "0"});
        when("it is shown", [&] {
            app.show();
            then("it serves a plain document root and is told the development path", [&] {
                require(captured.str().find(detail::developmentPathNote) != std::string::npos);
            });
        });
    });
}};

const Scenario developerApplicationNeeds{"The developer application needs an explicit project and a trusted environment", [] {
    using App = BasicDeveloperApp<QuietFrontend>;
    const DeveloperBootstrapOptions trusted{.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"};
    given("no project directory", [&] {
        App app({}, WebFrontConfig{.port = "0"});
        then("bootstrap is refused", [&] {
            requireThrows<std::invalid_argument>([&] { app.bootstrap("developer", trusted); });
        });
    });
    given("a project directory", [&] {
        TemporaryDirectory project;
        App                app(project.path(), WebFrontConfig{.port = "0"});
        then("an unknown mode or an unpinned environment is refused", [&] {
            requireThrows<std::invalid_argument>([&] { app.bootstrap("other", trusted); });
            requireThrows<std::invalid_argument>([&] { app.bootstrap("developer", {.environment = "https://example.test/env.tgz", .integrity = "md5-abc"}); });
            requireThrows<std::invalid_argument>([&] { app.bootstrap("developer", {.environment = "", .integrity = "sha512-abc"}); });
        });
        then("a pinned environment is accepted once", [&] {
            requireNothrow([&] { app.bootstrap("developer", trusted); });
            requireThrows<std::logic_error>([&] { app.bootstrap("developer", trusted); });
        });
        then("a registry that is neither HTTPS nor same-origin is refused", [&] {
            for (const auto* registry : {"http://registry.test/", "//registry.test/", "/\\registry.test/", "/mirror\\..\\x/", "registry/", ""}) {
                auto options     = trusted;
                options.registry = registry;
                requireThrows<std::invalid_argument>([&] { app.bootstrap("developer", options); });
            }
        });
        then("a manifest directory that is absolute, has an empty or dot component, or leaves the project is refused", [&] {
            for (const auto* directory : {"/etc", "a//b", "a/", "../outside", "a/../b", "a\\b", "C:/wf", "./wf", ".webfront", ".webfront/developer/preview"}) {
                auto options              = trusted;
                options.manifestDirectory = directory;
                requireThrows<std::invalid_argument>([&] { app.bootstrap("developer", options); });
            }
        });
        then("a relative manifest directory is accepted", [&] {
            auto options              = trusted;
            options.manifestDirectory = "js/developer";
            requireNothrow([&] { app.bootstrap("developer", options); });
        });
    });
}};

const Scenario environmentLearns{"The environment learns which registry and manifest directory the application chose", [] {
    const DeveloperBootstrapOptions trusted{.environment = "https://example.test/env.tgz", .integrity = "sha512-abc"};
    given("neither option", [&] {
        then("the default registry is passed and marked as not chosen, so the manifest's applies, and the manifest is in the "
             "project directory", [&] {
            require(developer::bootstrapConfig(trusted)
                    == R"({"environment":"https://example.test/env.tgz","integrity":"sha512-abc","registry":"https://registry.npmjs.org/","registryExplicit":false,"supported":{"protocol":[1],"api":[1]}})");
        });
    });
    given("a manifest directory chosen by the application", [&] {
        auto options              = trusted;
        options.manifestDirectory = "js/developer";
        then("it is passed, before the supported versions", [&] {
            require(developer::bootstrapConfig(options),
                         endsWith(R"("registryExplicit":false,"manifestDirectory":"js/developer","supported":{"protocol":[1],"api":[1]}})"));
        });
    });
    given("a same-origin mirror chosen by the application", [&] {
        auto options     = trusted;
        options.registry = "/mirror/";
        then("it is passed as the application's choice", [&] {
            require(developer::bootstrapConfig(options), endsWith(R"("registry":"/mirror/","registryExplicit":true,"supported":{"protocol":[1],"api":[1]}})"));
        });
    });
}};

namespace {
constexpr auto manifestPath = "webfront.project.json";
constexpr auto lockPath     = "webfront-packages.lock.json";
const std::vector<std::string> pair{manifestPath, lockPath};

void publishFile(developer::Store& store, const std::string& path, std::string_view text) {
    const auto id = store.begin(path, false);
    store.write(id, "output", 0, bytes(text));
    store.commit(id);
}

// Stages a new content for each path, then publishes them together from the state `expected`.
void publishGroup(developer::Store& store, const std::vector<std::string>& paths, const std::vector<std::string_view>& texts, const std::string& expected) {
    std::vector<std::string> ids;
    for (const auto& path : paths)
        ids.push_back(store.begin(path, false));
    for (std::size_t index = 0; index < ids.size(); ++index)
        store.write(ids[index], "output", 0, bytes(texts[index]));
    store.commitGroup(ids, paths, expected);
}

// Stages new contents for the manifest and the lock, then publishes them together.
void publishPair(developer::Store& store, std::string_view manifest, std::string_view lock, const std::string& expected) {
    publishGroup(store, pair, {manifest, lock}, expected);
}

// The lock file stays in the journal directory; anything else is a journal or its remains.
[[nodiscard]] bool journalsRemain(const std::filesystem::path& directory) {
    return std::ranges::any_of(std::filesystem::directory_iterator(directory),
                               [](const auto& entry) { return entry.path().filename() != developer::lockName; });
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
        requireThrows<developer::InjectedFault>([&] { publishPair(store, "manifest-2", "lock-2", expected); });
    }
    developer::Store reopened{project};
    return projectState(reopened);
}
}  // namespace

const Scenario manifestLock{"A manifest and its lock are published together or not at all", [] {
    TemporaryDirectory project;
    {
        developer::Store store{project.path()};
        publishFile(store, manifestPath, "manifest-1");
        publishFile(store, lockPath, "lock-1");
    }
    given("an installed manifest and lock", [&] {
        developer::Store store{project.path()};
        when("both files are published after an uninterrupted install", [&] {
            publishPair(store, "manifest-2", "lock-2", store.state(pair));
            then("both are replaced and no journal remains", [&] {
                require(projectState(store) == "manifest-2|lock-2");
                require(!journalsRemain(project.path() / ".webfront/developer/journal"));
            });
        });
        when("the process stops before the journal is committed", [&] {
            store.state(pair);
            then("the previous state is intact after restart", [&] {
                require(crashAt("before-journal", project.path()) == "manifest-1|lock-1");
            });
        });
        when("the process stops right after the journal is committed", [&] {
            then("restart completes the publication", [&] {
                require(crashAt("after-journal", project.path()) == "manifest-2|lock-2");
            });
        });
        when("the process stops between the two replacements", [&] {
            then("restart completes the publication", [&] {
                require(crashAt("replacement-1", project.path()) == "manifest-2|lock-2");
            });
        });
        when("the process stops once the replacements are durable but the journal is still there", [&] {
            then("restart completes the publication", [&] {
                require(crashAt("replacements-synced", project.path()) == "manifest-2|lock-2");
            });
        });
        when("the journal is removed", [&] {
            std::vector<std::string> steps;
            std::vector<bool>        journalPresent;
            {
                developer::Store observed{project.path(), developer::defaultAbandonedAfter, [&](std::string_view name) {
                                              steps.emplace_back(name);
                                              if (name == "replacements-synced")
                                                  journalPresent.push_back(journalsRemain(project.path() / ".webfront/developer/journal"));
                                          }};
                publishPair(observed, "manifest-2", "lock-2", observed.state(pair));
            }
            then("it is only after both replacements, and the journal was still present until then", [&] {
                require(steps == std::vector<std::string>{"before-journal", "after-journal", "replacement-1", "replacement-2", "replacements-synced"});
                require(journalPresent == std::vector<bool>{true});
            });
        });
        when("the process stops after the second replacement", [&] {
            then("restart completes the publication", [&] {
                require(crashAt("replacement-2", project.path()) == "manifest-2|lock-2");
            });
        });
    });
    given("a publication committed but interrupted between its replacements", [&] {
        {
            developer::Store store{project.path(), developer::defaultAbandonedAfter, [](std::string_view name) {
                                       if (name == "replacement-1")
                                           throw developer::InjectedFault{"crash"};
                                   }};
            requireThrows<developer::InjectedFault>([&] { publishPair(store, "manifest-2", "lock-2", store.state(pair)); });
        }
        when("the recovery itself is interrupted, once at each of its steps", [&] {
            for (const char* step : {"replacement-1", "replacement-2"}) {
                requireThrows<developer::InjectedFault>([&] { developer::Store(project.path(), developer::defaultAbandonedAfter,
                                                   [step](std::string_view name) {
                                                       if (name == step)
                                                           throw developer::InjectedFault{"crash"};
                                                   }); });
            }
            then("a later restart still reaches the new state", [&] {
                developer::Store store{project.path()};
                require(projectState(store) == "manifest-2|lock-2");
                require(!journalsRemain(project.path() / ".webfront/developer/journal"));
            });
        });
    });
    given("leftovers of a publication that never reached its commit point", [&] {
        const auto journal = project.path() / ".webfront/developer/journal";
        std::filesystem::create_directories(journal / "7.d");
        std::ofstream{journal / "7.d" / "0"} << "orphan";
        std::ofstream{journal / "7.tmp"} << "webfront-journal 1\nwebfront.project.json\n0\n";
        std::ofstream{journal / "8.journal"} << "webfront-journal 1\nwebfront.project.json\n0\n";
        when("the project is opened", [&] {
            developer::Store store{project.path()};
            then("they are abandoned and the project keeps its files", [&] {
                require(!journalsRemain(journal));
                require(projectState(store) == "manifest-1|lock-1");
            });
        });
    });
}};

const Scenario committedPublication{"A committed publication that cannot be completed does not prevent opening the project", [] {
    TemporaryDirectory project;
    const auto         journal = project.path() / ".webfront/developer/journal";
    {
        developer::Store store{project.path()};
        publishFile(store, manifestPath, "manifest-1");
        publishFile(store, lockPath, "lock-1");
    }
    given("a publication committed before a crash whose manifest replacement is now blocked", [&] {
        require(crashAt("after-journal", project.path()) == "manifest-2|lock-2");
        {
            // Commit a second publication, then obstruct its first target.
            developer::Store store{project.path(), developer::defaultAbandonedAfter, [](std::string_view name) {
                                       if (name == "after-journal")
                                           throw developer::InjectedFault{"crash"};
                                   }};
            requireThrows<developer::InjectedFault>([&] { publishPair(store, "manifest-3", "lock-3", store.state(pair)); });
        }
        std::filesystem::remove(project.path() / manifestPath);
        std::filesystem::create_directories(project.path() / manifestPath / "obstacle");
        when("the project is opened", [&] {
            developer::Store store{project.path()};
            then("it opens, reports the failure, refuses to read a state and keeps the journal", [&] {
                require(store.recoveryFailure().has_value());
                requireThrows<developer::IncompletePublication>([&] { store.state(pair); });
                require(journalsRemain(journal));
                require(read(store, lockPath) == "lock-2");
            });
            andWhen("the obstacle is removed", [&] {
                std::filesystem::remove_all(project.path() / manifestPath);
                then("the next state completes the publication", [&] {
                    store.state(pair);
                    require(!store.recoveryFailure().has_value());
                    require(projectState(store) == "manifest-3|lock-3");
                    require(!journalsRemain(journal));
                });
            });
        });
    });
    given("a replacement failing during the publication", [&] {
        bool             failOnce = true;
        developer::Store store{project.path(), developer::defaultAbandonedAfter, [&failOnce](std::string_view name) {
                                   if (name == "replacement-1" && std::exchange(failOnce, false))
                                       throw std::runtime_error("target locked");
                               }};
        when("the group is published", [&] {
            then("the caller learns that the change is committed but not applied, and the next state applies it", [&] {
                requireThrows<developer::IncompletePublication>([&] { publishPair(store, "manifest-2", "lock-2", store.state(pair)); });
                require(store.recoveryFailure().has_value());
                store.state(pair);
                require(projectState(store) == "manifest-2|lock-2");
                require(!journalsRemain(journal));
            });
        });
    });
    given("a failure before the commit point", [&] {
        developer::Store store{project.path(), developer::defaultAbandonedAfter, [](std::string_view name) {
                                   if (name == "before-journal")
                                       throw std::runtime_error("disk full");
                               }};
        const auto       first  = store.begin(manifestPath, false);
        const auto       second = store.begin(lockPath, false);
        store.write(first, "output", 0, bytes("manifest-2"));
        store.write(second, "output", 0, bytes("lock-2"));
        requireThrows([&] { store.commitGroup({first, second}, pair, store.state(pair)); }, equals("disk full"));
        then("the staged files are returned to their transactions, which still publish", [&] {
            require(projectState(store) == "manifest-1|lock-1");
            require(!journalsRemain(journal));
            store.commit(first);
            store.commit(second);
            require(projectState(store) == "manifest-2|lock-2");
        });
    });
}};

const Scenario publicationPrepared{"A publication prepared from stale state is refused", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    publishFile(store, manifestPath, "manifest-1");
    publishFile(store, lockPath, "lock-1");
    given("two browsers reading the same state", [&] {
        const auto firstBrowser  = store.state(pair);
        const auto secondBrowser = store.state(pair);
        require(firstBrowser == secondBrowser);
        when("the first publishes, then the second does with what it read", [&] {
            publishPair(store, "manifest-a", "lock-a", firstBrowser);
            then("the second is refused and nothing is lost", [&] {
                requireThrows([&] { publishPair(store, "manifest-b", "lock-b", secondBrowser); }, contains("changed since it was read"));
                require(projectState(store) == "manifest-a|lock-a");
            });
            andThen("it succeeds after reading the new state again", [&] {
                publishPair(store, "manifest-ab", "lock-ab", store.state(pair));
                require(projectState(store) == "manifest-ab|lock-ab");
            });
        });
        when("a file changes outside the store", [&] {
            publishFile(store, lockPath, "lock-c");
            then("the state differs and the group is refused", [&] {
                require(store.state(pair) != firstBrowser);
                requireThrows([&] { publishPair(store, "manifest-b", "lock-b", firstBrowser); });
            });
        });
    });
    given("a missing lock", [&] {
        std::filesystem::remove(project.path() / lockPath);
        then("its absence is part of the state", [&] {
            const auto absent = store.state(pair);
            publishFile(store, lockPath, "");
            require(store.state(pair) != absent);
        });
    });
    given("malformed groups", [&] {
        const auto first = store.begin(manifestPath, false);
        store.write(first, "output", 0, bytes("x"));
        then("they are refused without changing the project", [&] {
            requireThrows([&] { store.commitGroup({}, pair, store.state(pair)); });
            requireThrows([&] { store.commitGroup({first, first}, pair, store.state(pair)); });
            requireThrows([&] { store.commitGroup({first}, {lockPath}, store.state({lockPath})); });
            require(projectState(store) == "manifest-1|lock-1");
            store.abort(first);
        });
    });
}};

const Scenario newDirectories{"New directories are durable once their parents are synchronized", [] {
    TemporaryDirectory project;
    const auto         root = std::filesystem::canonical(project.path());
    given("a project without the journal directory", [&] {
        std::vector<std::filesystem::path> synced;
        when("the directories are created", [&] {
            developer::createDirectoriesDurably(root, root / ".webfront/developer/journal",
                                                [&](const std::filesystem::path& directory) {
                                                    // The directory just created must exist when its parent is synchronized.
                                                    require(std::filesystem::exists(directory / (synced.empty() ? ".webfront" : synced.size() == 1 ? "developer" : "journal")));
                                                    synced.push_back(directory);
                                                });
            then("each created directory has its parent synchronized, outermost first", [&] {
                require(synced == std::vector<std::filesystem::path>{root, root / ".webfront", root / ".webfront/developer"});
            });
        });
        when("some of them already exist", [&] {
            std::filesystem::create_directories(root / ".webfront/developer");
            developer::createDirectoriesDurably(root, root / ".webfront/developer/journal",
                                                [&](const std::filesystem::path& directory) { synced.push_back(directory); });
            then("only the missing one is created and synchronized", [&] {
                require(synced == std::vector<std::filesystem::path>{root / ".webfront/developer"});
            });
        });
    });
}};

namespace {
// Holds a thread at a step until opened. Opening is idempotent and also happens on destruction, so a failed
// assertion never leaves a thread blocked behind a future that waits for it.
class Gate {
public:
    void open() {
        std::call_once(once, [this] { opened.set_value(); });
    }
    void wait() const {
        future.wait();
    }

private:
    std::once_flag      once;
    std::promise<void>  opened;
    std::shared_future<void> future{opened.get_future().share()};
};
struct OpenOnExit {
    Gate& gate;
    ~OpenOnExit() {
        gate.open();
    }
};
}  // namespace

const Scenario secondOpener{"A second opener of the project waits for a publication in progress instead of abandoning it", [] {
    TemporaryDirectory project;
    {
        developer::Store store{project.path()};
        publishFile(store, manifestPath, "manifest-1");
        publishFile(store, lockPath, "lock-1");
    }
    given("a publication stopped between staging its files and writing its journal", [&] {
        Gate               reached;
        Gate               release;
        developer::Store   publisher{project.path(), developer::defaultAbandonedAfter, [&](std::string_view name) {
                                         if (name == "before-journal") {
                                             reached.open();
                                             release.wait();
                                         }
                                     }};
        const auto         before      = publisher.state(pair);
        auto               publication = std::async(std::launch::async, [&] {
            try {
                publishPair(publisher, "manifest-2", "lock-2", before);
            } catch (...) {
                reached.open();  // do not leave the test waiting for a step that is never reached
                throw;
            }
        });
        const OpenOnExit   unblock{release};
        reached.wait();
        when("another store, as another process would, opens the project meanwhile", [&] {
            auto       opening = std::async(std::launch::async, [&] { developer::Store other{project.path()}; return projectState(other); });
            const auto waiting = opening.wait_for(std::chrono::milliseconds(300));
            release.open();
            publication.get();
            then("it waited, then sees the completed publication", [&] {
                check(waiting == std::future_status::timeout);
                require(opening.get() == "manifest-2|lock-2");
            });
        });
        when("another store tries to publish from the state read before the first publication", [&] {
            // The refusal is read in its own thread: an exception object shared across threads by a future is
            // reported as a race by ThreadSanitizer, whose runtime does not see libc++abi's reference counting.
            auto competing = std::async(std::launch::async, [&]() -> std::string {
                try {
                    developer::Store other{project.path()};
                    publishPair(other, "manifest-3", "lock-3", before);
                    return "published";
                } catch (const std::exception& error) {
                    return error.what();
                }
            });
            const auto waiting = competing.wait_for(std::chrono::milliseconds(300));
            release.open();
            publication.get();
            then("its precondition is checked after the first publication, which wins", [&] {
                check(waiting == std::future_status::timeout);
                require(competing.get(), contains("changed since it was read"));
                require(projectState(publisher) == "manifest-2|lock-2");
            });
        });
    });
}};

const Scenario sha256Vectors{"SHA-256 matches the FIPS 180-4 vectors at every padding boundary, fed whole or in pieces", [] {
    using utils::Sha256;
    const auto digest = [](std::string_view text, std::size_t piece) {
        Sha256 hasher;
        for (std::size_t offset = 0; offset < text.size(); offset += piece)
            hasher.update(text.substr(offset, piece));
        return Sha256::hex(hasher.finish());
    };
    const std::string million(1000000, 'a');
    for (const auto& [text, expected] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
             {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
             {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
             {std::string_view(million).substr(0, 55), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
             {std::string_view(million).substr(0, 56), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
             {std::string_view(million).substr(0, 64), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
             {million, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}})
        for (const std::size_t piece : {std::size_t{1}, std::size_t{63}, std::size_t{4096}, std::max<std::size_t>(text.size(), 1)}) {
            auto scope = info("length ", text.size(), ", pieces of ", piece);
            require(digest(text, piece) == expected);
        }
}};

const Scenario projectDirectoryDigested{"A project directory is digested whole, its links marked and never followed", [] {
    TemporaryDirectory project;
    TemporaryDirectory outside;
    developer::Store   store{project.path()};
    publishFile(store, "ui/main.tsx", "main");
    publishFile(store, "ui/parts/a.ts", "a");
    publishFile(store, "ui/b.css", "b");
    const auto text = [&](std::string path) {
        const auto bytes = store.digests(std::move(path));
        return std::string(bytes.begin(), bytes.end());
    };
    const std::string expected = R"({"b.css":"3e23e8160039594a33894f6564e1b1348bbd7a0088d42c4acb73eeaed59c009d",)"
                                 R"("main.tsx":"0d6e4079e36703ebd37c00722f5891d28b0e2811dc114b129215123adcce3605",)"
                                 R"("parts/a.ts":"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"})";
    then("every file below the directory, nested ones included, maps its relative path to its SHA-256", [&] {
        require(text("ui") == expected);
        require(text("ui/parts") == R"({"a.ts":"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"})");
    });
    then("a missing directory, a file or a path leaving the project is refused", [&] {
        requireThrows([&] { store.digests("missing"); }, contains("Cannot list project directory"));
        requireThrows([&] { store.digests("ui/main.tsx"); }, contains("Cannot list project directory"));
        requireThrows<std::invalid_argument>([&] { store.digests("../outside"); });
    });
    given("an extra link to a file of the project, a link outside it and one back to the directory itself", [&] {
        std::ofstream(outside.path() / "secret.txt") << "secret";
        std::error_code fileError, outError, loopError;
        std::filesystem::create_symlink(project.path() / "ui" / "main.tsx", project.path() / "ui" / "alias.txt", fileError);
        std::filesystem::create_directory_symlink(outside.path(), project.path() / "ui" / "escape", outError);
        std::filesystem::create_directory_symlink(project.path() / "ui", project.path() / "ui" / "parts" / "loop", loopError);
        if (!fileError && !outError && !loopError) {
            then("each is a difference that no digest equals, and none is followed", [&] {
                require(text("ui") == R"({"alias.txt":"link","b.css":"3e23e8160039594a33894f6564e1b1348bbd7a0088d42c4acb73eeaed59c009d",)"
                                      R"("escape":"link","main.tsx":"0d6e4079e36703ebd37c00722f5891d28b0e2811dc114b129215123adcce3605",)"
                                      R"("parts/a.ts":"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb","parts/loop":"link"})");
            });
        }
    });
}};

const Scenario projectDirectoryListed{"A project directory is listed without leaving the project", [] {
    TemporaryDirectory project;
    TemporaryDirectory outside;
    developer::Store   store{project.path()};
    publishFile(store, "ui/main.tsx", "main");
    publishFile(store, "ui/parts/a.ts", "a");
    publishFile(store, "ui/b.css", "b");
    const auto text = [&](std::string path) {
        const auto bytes = store.list(std::move(path));
        return std::string(bytes.begin(), bytes.end());
    };
    given("files and a subdirectory", [&] {
        then("names are a sorted JSON array, and directories end with a slash", [&] {
            require(text("ui") == R"(["b.css","main.tsx","parts/"])");
            require(text("ui/parts") == R"(["a.ts"])");
            require(text(""), contains("ui/"));
        });
    });
    given("a link to a directory outside the project", [&] {
        std::error_code error;
        std::filesystem::create_directory_symlink(outside.path(), project.path() / "ui" / "escape", error);
        if (!error) {
            then("it is omitted, and listing through it is refused", [&] {
                require(text("ui") == R"(["b.css","main.tsx","parts/"])");
                requireThrows<std::invalid_argument>([&] { store.list("ui/escape"); });
            });
        }
    });
    given("a link whose target cannot be resolved, because it points to itself", [&] {
        std::error_code error;
        std::filesystem::create_symlink(project.path() / "ui" / "loop", project.path() / "ui" / "loop", error);
        if (!error) {
            then("only that entry is omitted", [&] {
                require(text("ui") == R"(["b.css","main.tsx","parts/"])");
            });
        }
    });
#ifndef _WIN32
    given("a file whose name contains a line break", [&] {
        std::ofstream(project.path() / "ui" / "a\nb").put('x');
        then("it is one escaped entry, not two", [&] {
            require(text("ui") == R"(["a\u000ab","b.css","main.tsx","parts/"])");
        });
    });
#endif
    given("a missing directory, a file or a path escaping the project", [&] {
        then("listing fails", [&] {
            requireThrows([&] { store.list("missing"); }, contains("Cannot list project directory"));
            requireThrows([&] { store.list("ui/main.tsx"); }, contains("Cannot list project directory"));
            requireThrows<std::invalid_argument>([&] { store.list("../outside"); });
        });
    });
}};

const Scenario projectPaths{"Project paths beyond the 260 characters of Windows are published, grouped and read like any other", [] {
    TemporaryDirectory project;
    developer::Store   store{project.path()};
    std::string        directory = "deep";  // absolute paths below it exceed MAX_PATH; none of its directories exists yet
    while (project.path().native().size() + directory.size() < 300)
        directory += "/" + std::string(40, 'd');
    when("a file is published there", [&] {
        const auto destination = directory + "/file.txt";
        publishFile(store, destination, "long");
        then("it can be read back and replaced", [&] {
            require(read(store, destination) == "long");
            publishFile(store, destination, "longer");
            require(read(store, destination) == "longer");
        });
        then("a path escaping the project is still refused", [&] {
            requireThrows<std::invalid_argument>([&] { store.read(directory + "/../../../../../../../../../../../../../../../../../outside", 0); });
        });
    });
    when("two files are published there as one group", [&] {
        const std::vector<std::string> paths{directory + "/a.txt", directory + "/b.txt"};
        publishGroup(store, paths, {"one", "two"}, store.state(paths));
        then("both are readable and no journal remains", [&] {
            require(read(store, paths[0]) == "one");
            require(read(store, paths[1]) == "two");
            require(!journalsRemain(project.path() / ".webfront/developer/journal"));
        });
    });
}};

const Scenario projectWhose{"A project whose own directory exceeds the 260 characters of Windows can be opened", [] {
    TemporaryDirectory outer;
    auto               root = outer.path();
    while (root.native().size() < 302)
        root /= std::string(40, 'r');
    std::filesystem::create_directories(developer::extendedPath(root));
    given("a store on that directory", [&] {
        developer::Store store{root};
        when("a file is published and a group is committed", [&] {
            publishFile(store, "file.txt", "root");
            const std::vector<std::string> paths{"sub/a.txt", "sub/b.txt"};
            publishGroup(store, paths, {"one", "two"}, store.state(paths));
            then("everything is readable and the project stays confined", [&] {
                require(read(store, "file.txt") == "root");
                require(read(store, "sub/b.txt") == "two");
                requireThrows<std::invalid_argument>([&] { store.read("../outside", 0); });
            });
        });
    });
    given("a developer filesystem on that directory", [&] {
        then("it opens", [&] {
            requireNothrow([&] { fs::DeveloperFS{root}; });
        });
    });
}};

#ifdef _WIN32
const Scenario faultingMemory{"Published files are copied from memory that may fault, reporting a failed read by value", [] {
    given("a committed page, a page that cannot be read and a guard page", [&] {
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        const auto page   = static_cast<std::size_t>(info.dwPageSize);
        auto*      region = static_cast<char*>(::VirtualAlloc(nullptr, 3 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        require(region != nullptr);
        struct Free {
            char* region;
            ~Free() {
                ::VirtualFree(region, 0, MEM_RELEASE);
            }
        } free{region};
        std::memset(region, 'x', page);
        DWORD previous = 0;
        require(::VirtualProtect(region + page, page, PAGE_NOACCESS, &previous) != 0);
        require(::VirtualProtect(region + 2 * page, page, PAGE_READWRITE | PAGE_GUARD, &previous) != 0);
        std::vector<char> out(2 * page);
        then("a readable range is copied", [&] {
            check(fs::detail::copyReadable(out.data(), region, page));
            check(out[0] == 'x');
            check(out[page - 1] == 'x');
        });
        then("a range that reaches an unreadable page fails without an exception", [&] {
            check(!fs::detail::copyReadable(out.data(), region, 2 * page));
            check(!fs::detail::copyReadable(out.data(), region + page, 1));
        });
        then("a guard page does not raise a structured exception either", [&] {
            check(!fs::detail::copyReadable(out.data(), region + 2 * page, 1));
        });
    });
    given("a published file of 200 kB", [&] {
        TemporaryDirectory project;
        developer::Store   store{project.path()};
        const auto         id = store.begin(".webfront/developer/preview", true);
        const std::string  content(200000, 'p');
        store.write(id, "big.bin", 0, bytes(content));
        store.commit(id);
        fs::DeveloperFS filesystem{project.path()};
        then("it is read whole through its mapped windows, whatever the block size", [&] {
            auto file = filesystem.open("/webfront/developer/preview/big.bin");
            require(file.has_value());
            std::string       got;
            std::vector<char> block(70001);
            for (std::size_t n; (n = file->read(block)) != 0;)
                got.append(block.data(), n);
            check(got == content);
        });
    });
}};
#endif
