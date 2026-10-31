#include "../../examples/life/Life.hpp"
#include "../unit/TemporaryDirectory.hpp"
#include <webfront/Commands.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace {
std::string readFixtureText(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 16 * 1024 * 1024)
        throw std::length_error("Fixture text exceeds its byte limit");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read fixture text");
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty() && !file.read(text.data(), static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Truncated fixture text");
    return text;
}

class FixturesFS {
public:
    explicit FixturesFS(std::filesystem::path projectRoot) : root(std::filesystem::canonical(projectRoot)) {}
    std::optional<webfront::fs::File> open(std::filesystem::path path) const {
        const auto relative = path.relative_path().generic_string();
        if (!relative.starts_with("fixtures/"))
            return {};
        std::ifstream stream(webfront::developer::projectPath(root, relative), std::ios::binary);
        if (stream)
            return webfront::fs::File{std::move(stream)};
        return {};
    }

private:
    std::filesystem::path root;
};

std::string readIntegrity(const std::filesystem::path& config) {
    std::string            integrity;
    const std::string      json = readFixtureText(config);
    webfront::json::Reader reader{json};
    reader.object([&](const std::string& key) {
        if (key == "integrity")
            integrity = reader.string();
        else
            reader.skipValue();
    });
    return integrity;
}

template <typename App>
void registerCallbacks(App& app, const std::filesystem::path& project, std::atomic<bool>& passed) {
    app.cppFunction("life.step", &webfront::demo::nextLife);
    app.cppFunction("test.header", [project](std::string name) {
        if (name != "LifeUI")
            throw std::invalid_argument("Unknown generated fixture");
        const auto header = readFixtureText(project / "generated" / (name + ".hpp"));
        // The generated header is larger than a bridge string: return a stable small digest.
        return std::to_string(std::hash<std::string>{}(header));
    });
    // The result a page sends after a non-interactive wf command (runNonInteractive), kept for the test to read back.
    struct Result {
        std::mutex  mutex;
        std::string json;
    };
    auto result = std::make_shared<Result>();
    app.cppFunction("test.result", [result](std::string json) {
        std::scoped_lock lock{result->mutex};
        result->json = std::move(json);
    });
    app.cppFunction("test.lastResult", [result] {
        std::scoped_lock lock{result->mutex};
        return result->json;
    });
    app.cppFunction("test.progress", [](std::string message) {
        std::cout << "Developer integration: " << message << std::endl;
    });
    app.cppFunction("test.report", [&passed](bool success, std::string message) {
        std::cout << message << std::endl;
        passed.store(success);
    });
}

// What the test's long commands went through, by tag: running, stopping (a lingering command after Ctrl+C), cancelled.
class CommandStates {
public:
    void set(const std::string& tag, std::string state) {
        std::scoped_lock lock{mutex};
        states[tag] = std::move(state);
    }
    void release(const std::string& tag) {
        std::scoped_lock lock{mutex};
        releasedTags.insert(tag);
    }
    void releaseAll() {
        std::scoped_lock lock{mutex};
        allReleased = true;
    }
    bool released(const std::string& tag) const {
        std::scoped_lock lock{mutex};
        return allReleased || releasedTags.contains(tag);
    }
    std::string get(const std::string& tag) const {
        std::scoped_lock lock{mutex};
        const auto       found = states.find(tag);
        return found == states.end() ? "absent" : found->second;
    }

private:
    mutable std::mutex                 mutex;
    std::map<std::string, std::string> states;
    std::set<std::string>              releasedTags;
    bool                               allReleased = false;
};

namespace ex = webfront::networking::ex;
using namespace std::chrono_literals;

// Marks its command cancelled once the coroutine unwinds or returns.
struct Cancelled {
    std::shared_ptr<CommandStates> states;
    std::string                    tag;
    ~Cancelled() {
        states->set(tag, "cancelled");
    }
};

template <typename Context>
webfront::Task waitUntilStopped(Context context, std::shared_ptr<CommandStates> states) {
    const auto tag = context.args().at(0);
    Cancelled  cancelled{states, tag};
    states->set(tag, "running");
    context.write("waiting " + tag + "\n");
    for (;;)
        co_await context.ui().resumeAfter(20ms);
}

// After Ctrl+C, remains deaf to the stop request until the test releases it.
template <typename Context>
webfront::Task lingerAfterStop(Context context, std::shared_ptr<CommandStates> states) {
    const auto tag = context.args().at(0);
    Cancelled  cancelled{states, tag};
    states->set(tag, "running");
    const auto deafPause = [&] {
        return ex::write_env(context.ui().resumeAfter(20ms), ex::prop{ex::get_stop_token, ex::never_stop_token{}});
    };
    while (!context.stopRequested())
        co_await deafPause();
    states->set(tag, "stopping");
    while (!states->released(tag))
        co_await deafPause();
}

template <typename App>
void registerCommands(webfront::Commands<App>& commands, const std::shared_ptr<CommandStates>& states) {
    using Context = typename webfront::Commands<App>::Context;
    commands.add("echo", "Write its arguments", [](Context context) -> webfront::Task {
        std::string line;
        for (const auto& word : context.args())
            line += (line.empty() ? "" : " ") + word;
        context.write(line + "\n");
        co_return;
    });
    commands.add("stream", "Write 150001 bytes", [](Context context) -> webfront::Task {
        std::string text;
        for (int i = 0; i < 75000; ++i)
            text += "\xC3\xA9";
        context.write(text + "\n");
        co_return;
    });
    commands.add("sleep", "Wait some milliseconds", [](Context context) -> webfront::Task {
        co_await context.ui().resumeAfter(std::chrono::milliseconds(std::stoi(context.args().at(0))));
    });
    commands.add("crlf", "Split a CRLF between two writes", [](Context context) -> webfront::Task {
        context.write("one\r");
        context.write("\ntwo\n");
        co_return;
    });
    commands.add("partial", "Write without a final line break", [](Context context) -> webfront::Task {
        context.write("partial");
        co_return;
    });
    commands.add("fail", "Throw", [](Context) -> webfront::Task {
        throw std::runtime_error("command failed on purpose");
    });
    commands.add("status", "Exit with a code", [](Context context) -> webfront::Task {
        context.exit(std::stoi(context.args().at(0)));
        co_return;
    });
    commands.add("wait", "Wait until stopped", [states](Context context) {
        return waitUntilStopped(std::move(context), states);
    });
    commands.add("linger", "Linger after Ctrl+C", [states](Context context) {
        return lingerAfterStop(std::move(context), states);
    });
}

// What a run does: the public smoke test, one part of the test (each part starts from its own project and runs alongside
// the others), or everything in one session when nothing is given.
struct Mode {
    bool        publicDistribution = false;
    std::string part               = "all";
    bool        runs(std::string_view name) const {
        return part == "all" || part == name;
    }
};

Mode parseMode(std::string_view argument) {
    Mode mode;
    if (argument.empty())
        return mode;
    if (argument == "--public") {
        mode.publicDistribution = true;
        mode.part               = "app";
    } else if (argument.starts_with("--part=")) {
        mode.part = std::string(argument.substr(7));
        if (mode.part != "app" && mode.part != "scripts" && mode.part != "typescript" && mode.part != "jasmine" && mode.part != "terminal")
            throw std::invalid_argument("Unknown part of the developer test: " + mode.part);
    } else {
        throw std::invalid_argument("Unknown smoke-test mode");
    }
    return mode;
}

template <typename App>
void bootstrapApp(App& app, const Mode& mode, const std::string& integrity) {
    if (mode.publicDistribution)
        app.bootstrap("developer");
    else
        app.bootstrap("developer", {.environment = "/fixtures/environment.tgz", .integrity = integrity, .registry = "/fixtures/registry/"});
}

void copyHeaders(const std::filesystem::path& project, const std::filesystem::path& output) {
    std::filesystem::create_directories(output);
    std::filesystem::copy_file(project / "generated/LifeUI.hpp", output / "LifeUI.hpp", std::filesystem::copy_options::overwrite_existing);
}

constexpr auto runScript =
    "import('/fixtures/run.mjs').catch(async e => { await webFront.cppFunction('test.report')(false, String(e.stack || e)); window.close(); });";
}  // namespace

int main(int argc, char** argv) {
    try {
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        if (argc != 3 && argc != 4)
            throw std::invalid_argument("Expected fixture directory, header output directory and optional --public or --part=<name>");
        const Mode mode = parseMode(argc == 4 ? std::string_view(argv[3]) : std::string_view{});
        webfront::test::TemporaryDirectory project;
        std::filesystem::copy(argv[1], project.path() / "fixtures", std::filesystem::copy_options::recursive);
        const auto integrity = readIntegrity(project.path() / "fixtures/provision.json");
        using App            = webfront::BasicDeveloperApp<WEBFRONT_TEST_FRONTEND, FixturesFS>;
        // A part has no port of its own to keep: the parts run at the same time, each on the port the system gives it.
        webfront::WebFrontConfig config;
        if (mode.part != "all")
            config.port = "0";
        App               app{project.path(), config};
        std::atomic<bool> passed{false};
        registerCallbacks(app, project.path(), passed);
        auto               states = std::make_shared<CommandStates>();
        webfront::Commands commands{app};
        registerCommands(commands, states);
        app.cppFunction("test.command", [states](std::string tag) {
            return states->get(tag);
        });
        app.cppFunction("test.release", [states](std::string tag) {
            states->release(tag);
        });
        app.cppFunction("test.part", [part = mode.part] {
            return part;
        });
        app.cppFunction("test.browsers", [&app] {
            return static_cast<double>(app.connectedBrowsers());
        });
        bootstrapApp(app, mode, integrity);
        auto subscription = app.subscribeUIStarted([injected = false](App::UI ui) mutable {
            if (!std::exchange(injected, true))
                ui.addScript(runScript);
        });
        try {
            app.show();
        } catch (...) {
            states->releaseAll();
            throw;
        }
        // The test ends while the "final" command runs: shutdown drains it.
        states->releaseAll();
        app.shutdown();
        if (!passed.load())
            return 1;
        // The command running at shutdown belongs to the terminal part; the generated header, to the application part.
        if (mode.runs("terminal") && !mode.publicDistribution && states->get("final") != "cancelled") {
            std::cerr << "The command running at shutdown was not drained: " << states->get("final") << '\n';
            return 1;
        }
        if (mode.runs("app"))
            copyHeaders(project.path(), argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
