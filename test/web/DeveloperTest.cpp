#include "../../examples/life/Life.hpp"
#include "../driver/DriverSuite.hpp"
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
#include <thread>
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
    explicit FixturesFS(std::filesystem::path projectRoot) : root(std::filesystem::canonical(projectRoot)) { }
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
void registerCallbacks(App& app, const std::filesystem::path& project, std::atomic<bool>& passed, std::function<void()> reported = {}) {
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
    app.cppFunction("test.report", [&passed, reported = std::move(reported)](bool success, std::string message) {
        std::cout << message << std::endl;
        passed.store(success);
        if (reported)
            reported();
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
        if (mode.part != "app" && mode.part != "scripts" && mode.part != "typescript" && mode.part != "jasmine" && mode.part != "terminal"
            && mode.part != "workspace")
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
    "import('/fixtures/run.mjs').catch(async e => { await webFront.cppFunction('test.report')(false, String(e.stack || e));"
    " if (location.search === '?developer-session' && parent !== self) parent.postMessage('webfront-developer-finished', new URL(document.referrer).origin);"
    " else window.close(); });";

// A part keeps its original project, origin, bridge and command ownership. Only the native window is shared.
class Part {
    using App = webfront::BasicDeveloperApp<WEBFRONT_TEST_FRONTEND, FixturesFS>;

public:
    Part(const std::filesystem::path& fixtures, Mode selected, webfront::testdriver::DriverDeadline* deadline = nullptr)
        : app(project.path(), {.port = "0"}), mode(std::move(selected)), states(std::make_shared<CommandStates>()), commands(app) {
        std::filesystem::copy(fixtures, project.path() / "fixtures", std::filesystem::copy_options::recursive);
        registerCallbacks(app, project.path(), passed, [this, deadline] {
            if (reported.exchange(true))
                throw std::runtime_error("A developer part reported twice");
            if (deadline)
                deadline->beginCase(180s);
            caseDeadline.endCase();
        });
        registerCommands(commands, states);
        app.cppFunction("test.command", [owned = states](std::string tag) {
            return owned->get(tag);
        });
        app.cppFunction("test.release", [owned = states](std::string tag) {
            owned->release(tag);
        });
        app.cppFunction("test.part", [part = mode.part] {
            return part;
        });
        app.cppFunction("test.browsers", [this] {
            return static_cast<double>(app.connectedBrowsers());
        });
        bootstrapApp(app, mode, readIntegrity(project.path() / "fixtures/provision.json"));
        subscription = app.subscribeUIStarted([this, injected = false](App::UI ui) mutable {
            if (!std::exchange(injected, true)) {
                caseDeadline.beginCase(180s);
                ui.addScript(runScript);
            }
        });
    }

    ~Part() {
        watchdog.request_stop();
        states->releaseAll();
        app.stop();
        if (server.joinable())
            server.join();
        // Drain while the commands, callback state and project are still alive.
        try {
            app.shutdown();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
    }

    void start() {
        watchdog = std::jthread([this](std::stop_token stop) {
            if (caseDeadline.expired(stop)) {
                std::cerr << "Developer part exceeded its native deadline: " << mode.part << '\n';
                std::cerr.flush();
                std::_Exit(1);
            }
        });
        server   = std::jthread([this] {
            app.run();
        });
    }
    void show() {
        app.show();
    }
    std::string origin() const {
        return "http://127.0.0.1:" + std::string(app.port());
    }
    std::string_view port() const {
        return app.port();
    }
    const std::filesystem::path& directory() const {
        return project.path();
    }

    bool finish(const std::filesystem::path& output) {
        watchdog.request_stop();
        states->releaseAll();
        app.stop();
        if (server.joinable())
            server.join();
        app.shutdown();
        bool success = reported.load() && passed.load();
        if (success && mode.runs("terminal") && !mode.publicDistribution && states->get("final") != "cancelled") {
            std::cerr << "The command running at shutdown was not drained: " << states->get("final") << '\n';
            success = false;
        }
        if (success && mode.runs("app"))
            copyHeaders(project.path(), output);
        return success;
    }

private:
    webfront::test::TemporaryDirectory   project;
    App                                  app;
    Mode                                 mode;
    std::atomic<bool>                    passed{false}, reported{false};
    std::shared_ptr<CommandStates>       states;
    webfront::Commands<App>              commands;
    webfront::UISubscription             subscription;
    webfront::testdriver::DriverDeadline caseDeadline{540s};
    std::jthread                         watchdog;
    std::jthread                         server;
};

int runSession(const std::filesystem::path& fixtures, const std::filesystem::path& output) {
    const auto                 started = std::chrono::steady_clock::now();
    constexpr std::string_view names[]{"app", "scripts", "typescript", "jasmine", "terminal", "workspace"};
    std::filesystem::create_directories(output / "results");
    // Remove only this invocation's own reports, so a crash cannot leave a previous success behind.
    for (const auto name : names)
        std::filesystem::remove(output / "results" / (std::string(name) + ".json"));
    webfront::testdriver::DriverDeadline deadlines(540s);
    deadlines.beginCase(180s);
    std::vector<std::unique_ptr<Part>> parts;
    webfront::ContentPolicy            policy;
    std::string                        urls = "[";
    for (const auto name : names) {
        auto part = std::make_unique<Part>(fixtures, Mode{false, std::string(name)}, &deadlines);
        policy.navigationAllowedOrigins.push_back(part->origin());
        urls += (urls.size() > 1 ? "," : "") + webfront::json::quote(part->origin() + "/webfront-bootstrap.html?developer-session");
        parts.push_back(std::move(part));
    }
    urls              += "]";
    const auto    page = parts.front()->directory() / "fixtures/developer-suite.html";
    std::ofstream html(page, std::ios::binary);
    html << R"HTML(<!doctype html><meta charset="utf-8"><title>Developer integration</title>
<style>html,body{margin:0;width:100%;height:100%;background:#141821;color:#eee;font:14px sans-serif}
body{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));grid-auto-rows:minmax(0,1fr);gap:2px}
section{min-width:0;min-height:0}header{height:28px;line-height:28px;padding:0 10px}
iframe{display:block;border:0;width:100%;height:calc(100% - 28px)}</style><body><script>
const urls = )HTML"
         << urls << R"HTML(;
const names = ['application', 'scripts', 'TypeScript', 'Jasmine', 'terminal', 'workspace'];
let cursor = 0, completed = 0;
const frames = new Map();
function launch(index) {
    const pane = document.createElement('section'), title = document.createElement('header'), frame = document.createElement('iframe');
    title.textContent = `Developer integration: ${names[index]}`;
    // Give the embedded page the focus a standalone native page had, before its asynchronous bootstrap mounts the preview.
    frame.addEventListener('load', () => frame.focus());
    frame.src = urls[index]; pane.append(title, frame); document.body.append(pane);
    frames.set(index, {pane, frame});
}
function advance() {
    // All running pages remain displayed. Workspace's layout/rendering checks get the entire window afterwards.
    while (cursor < 5 && frames.size < 4) launch(cursor++);
    if (completed === 5 && !frames.size) {
        document.body.style.gridTemplateColumns = '1fr';
        launch(cursor++);
    }
    if (completed === urls.length) window.close();
}
addEventListener('message', event => {
    for (const [index, {pane, frame}] of frames) {
        if (event.source !== frame.contentWindow || event.origin !== new URL(urls[index]).origin || event.data !== 'webfront-developer-finished') continue;
        pane.remove(); frames.delete(index); ++completed; advance(); break;
    }
});
advance();
</script>)HTML";
    html.close();
    if (!html)
        throw std::runtime_error("Cannot write developer session page");
    for (auto& part : parts)
        part->start();
    // The native bound applies even when a renderer blocks its JavaScript timers.
    std::jthread watchdog([&](std::stop_token stop) {
        if (deadlines.expired(stop)) {
            std::cerr << "Developer session exceeded its per-part deadline\n";
            std::cerr.flush();
            std::_Exit(1);
        }
    });
    std::cout << "Developer session: one visible native window, six isolated pages\n";
    webfront::frontend::detail::openWith<WEBFRONT_TEST_FRONTEND>("127.0.0.1", parts.front()->port(), "fixtures/developer-suite.html", policy, {});
    watchdog.request_stop();
    bool passed = true;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool    success = parts[i]->finish(output);
        std::ofstream report(output / "results" / (std::string(names[i]) + ".json"));
        report << "{\"exitCode\":" << (success ? 0 : 1) << ",\"output\":\"Developer part " << names[i] << "\"}";
        report.close();
        if (!report)
            throw std::runtime_error("Cannot write developer part report");
        passed = success && passed;
    }
    std::cout << "Developer session: nativeWindows=1 parts=6 durationMs="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << '\n';
    return passed ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        // CTest shows the output of a failed case only: it then names every request and the status it was answered,
        // which is what tells a resource the page could not load from one the server refused (#542).
        webfront::log::setLogLevel(webfront::log::Info);
        [[maybe_unused]] const auto requestLog = webfront::log::addSinks(webfront::log::clogSink);
        if (argc != 3 && argc != 4)
            throw std::invalid_argument("Expected fixture directory, header output directory and optional --session, --public or --part=<name>");
        if (argc == 4 && std::string_view(argv[3]) == "--session")
            return runSession(argv[1], argv[2]);
        const Mode mode = parseMode(argc == 4 ? std::string_view(argv[3]) : std::string_view{});
        Part       part(argv[1], mode);
        part.show();
        return part.finish(argv[2]) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
