// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-10-20
//
// WebFront — tests/web
// Role: Integration test of the developer environment journey: bootstrap, project creation and wf commands in a real browser.
// Thread: ui-loop | Alloc: unrestricted
// LLM: real-browser test; fixtures are served locally, no public network

#include "../../examples/life/Life.hpp"
#include "../driver/RequestLog.hpp"
#include "../unit/TemporaryDirectory.hpp"
#include <webfront/Commands.hpp>
#include <webfront/utils/Json.hpp>
#include <webfront/WebFront.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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

// What the test's long commands went through, by tag: running, stopping (a lingering command after an interruption), cancelled.
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

// After an interruption, remains deaf to the stop request until the test releases it.
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
    commands.add("linger", "Linger after an interruption", [states](Context context) {
        return lingerAfterStop(std::move(context), states);
    });
}

// What a run does: the public smoke test, one part of the test (each part starts from its own project and runs alongside
// the others), or everything in one project when nothing is given.
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

#ifdef WEBFRONT_EMBED_CEF
// One key pressed and released in the CEF view as the keyboard sends it, through DevTools (Input.dispatchKeyEvent): the
// events are trusted, so the engine runs its own copy and paste for Ctrl+C and Ctrl+V, which a script's synthetic events
// never trigger (#721). Runs on the interface thread, where the view's browser is published.
template <typename App>
class KeyPressTask : public CefTask {
public:
    KeyPressTask(App& application, std::vector<std::tuple<std::string, std::string, int, int>> pressed, std::shared_ptr<std::promise<std::string>> result)
        : app(application), keys(std::move(pressed)), done(std::move(result)) { }

    void Execute() override {
        const auto window = app.nativeWindow().get();
        if (!window || !window->pointer) {
            done->set_value("the view is not open");
            return;
        }
        const auto host = static_cast<CefBrowser*>(window->pointer)->GetHost();
        for (const auto& [key, code, keyCode, modifiers] : keys)
            for (const auto* type : {"rawKeyDown", "keyUp"}) {
                auto params = CefDictionaryValue::Create();
                params->SetString("type", type);
                params->SetString("key", key);
                params->SetString("code", code);
                params->SetInt("windowsVirtualKeyCode", keyCode);
                params->SetInt("nativeVirtualKeyCode", keyCode);
                params->SetInt("modifiers", modifiers);
                if (!host->ExecuteDevToolsMethod(0, "Input.dispatchKeyEvent", params)) {
                    done->set_value("the engine refused a key event");
                    return;
                }
            }
        done->set_value("sent");
    }

private:
    App&                                                        app;
    std::vector<std::tuple<std::string, std::string, int, int>> keys;
    std::shared_ptr<std::promise<std::string>>                  done;

    #ifdef __clang__
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wextra-semi"
    #endif
    IMPLEMENT_REFCOUNTING(KeyPressTask);
    #ifdef __clang__
        #pragma clang diagnostic pop
    #endif
};
#endif

/// Presses keys in the view as the keyboard does: [[key, code, windowsKeyCode, modifiers], ...], modifiers as DevTools
/// counts them (2 for Ctrl). "sent", "unsupported" on a frontend other than CEF, or why the keys could not be sent.
template <typename App>
std::string pressKeys([[maybe_unused]] App& app, const std::string& json) {
    std::vector<std::tuple<std::string, std::string, int, int>> keys;
    webfront::json::Reader                                      reader{json};
    reader.array([&] {
        std::vector<std::string> fields;
        reader.array([&] {
            fields.push_back(fields.size() < 2 ? reader.string() : std::string(reader.rawValue()));
        });
        if (fields.size() != 4)
            throw std::invalid_argument("A key is [key, code, keyCode, modifiers]");
        keys.emplace_back(fields[0], fields[1], std::stoi(fields[2]), std::stoi(fields[3]));
    });
#ifdef WEBFRONT_EMBED_CEF
    if constexpr (std::is_same_v<WEBFRONT_TEST_FRONTEND, webfront::frontend::CEFFrontend>) {
        auto done   = std::make_shared<std::promise<std::string>>();
        auto result = done->get_future();
        if (!CefPostTask(TID_UI, new KeyPressTask<App>(app, std::move(keys), done)))
            return "the interface thread is gone";
        if (result.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            return "the interface thread did not send the keys";
        return result.get();
    }
#endif
    return "unsupported";
}

constexpr auto runScript =
    "import('/fixtures/run.mjs').catch(async e => { await webFront.cppFunction('test.report')(false, String(e.stack || e)); window.close(); });";

// A part is a process of its own: its project, its origin, its bridge, its commands and its visible window.
class Part {
    using App = webfront::BasicDeveloperApp<WEBFRONT_TEST_FRONTEND, FixturesFS>;

public:
    Part(const std::filesystem::path& fixtures, Mode selected)
        : app(project.path(), configFor(selected)), mode(std::move(selected)), states(std::make_shared<CommandStates>()), commands(app) {
        std::filesystem::copy(fixtures, project.path() / "fixtures", std::filesystem::copy_options::recursive);
        registerCallbacks(app, project.path(), passed, [this] {
            std::scoped_lock lock{reportMutex};
            if (reported)
                throw std::runtime_error("A developer part reported twice");
            reported = true;
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
        app.cppFunction("test.keys", [this](std::string keys) {
            return pressKeys(app, keys);
        });
        // What the inspectors must offer this page, known from the frontend type and not from webfront.inspector.containers(),
        // which the spec checks against it: the view of an embedded frontend is admitted, and a frontend that has secondary
        // windows offers one (ADR-027, sections 2 and 8).
        app.cppFunction("test.expectedContainers", [] {
            return std::tuple<bool, bool>{App::supportsSecondaryWindow, true};
        });
        app.cppFunction("test.browsers", [this] {
            return static_cast<double>(app.connectedBrowsers());
        });
        // The connections that proved they are a view of the application: the window of show() and its secondary windows.
        app.cppFunction("test.views", [this] {
            double views = 0;
            app.forEachUI([&views](App::UI ui) {
                if (ui.isEmbeddedView())
                    ++views;
            });
            return views;
        });
        bootstrapApp(app, mode, readIntegrity(project.path() / "fixtures/provision.json"));
        // A watch that the page declares and removes while an inspector shows the watches (#680): it appears, then disappears.
        app.cppFunction("test.watch", [this](std::string name) {
            watches.insert_or_assign(name, app.watch(name, [this]() -> const std::string& {
                return mode.part;
            }));
        });
        app.cppFunction("test.unwatch", [this](std::string name) {
            watches.erase(name);
        });
        // A call that answers only once the page lets it go: the Bridge panel shows it pending meanwhile (#684).
        app.cppFunction("test.hold", [this] {
            held = true;
            return answerOnceLetGo(&app, &held);
        });
        app.cppFunction("test.letGo", [this] {
            held = false;
        });
        subscription = app.subscribeUIStarted([injected = false](App::UI ui) mutable {
            if (!std::exchange(injected, true))
                ui.addScript(runScript);
        });
    }

    /// Completes once `holding` is false; after thirty seconds at most, so that a failed spec does not keep the call for ever.
    static webfront::ResultTask<void> answerOnceLetGo(App* application, std::atomic<bool>* holding) {
        for (int waits = 0; waits < 3000 && holding->load(); ++waits)
            co_await application->resumeAfter(std::chrono::milliseconds(10));
    }

    ~Part() {
        watches.clear();
        states->releaseAll();
        app.stop();
        // Drain while the commands, callback state and project are still alive.
        try {
            app.shutdown();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
    }

    void show() {
        app.show();
    }

    bool finish(const std::filesystem::path& output) {
        states->releaseAll();
        app.stop();
        app.shutdown();
        const std::scoped_lock lock{reportMutex};
        if (!reported) {
            std::cerr << "Developer part " << mode.part << " closed without reporting a result\n";
            return false;
        }
        if (!passed.load())
            return false;
        if (mode.runs("terminal") && !mode.publicDistribution && states->get("final") != "cancelled") {
            std::cerr << "The command running at shutdown was not drained: " << states->get("final") << '\n';
            return false;
        }
        if (mode.runs("app")) {
            try {
                copyHeaders(project.path(), output);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
                return false;
            }
        }
        return true;
    }

private:
    webfront::test::TemporaryDirectory project;
    App                                app;
    Mode                               mode;
    static webfront::WebFrontConfig    configFor(const Mode& selected) {
        webfront::WebFrontConfig config;
        // A part has no port of its own to keep: the parts run at the same time, each on the port the system gives it.
        if (selected.part != "all")
            config.port = "0";
        return config;
    }

    std::atomic<bool>                               passed{false};
    bool                                            reported = false;
    std::mutex                                      reportMutex;
    std::shared_ptr<CommandStates>                  states;
    webfront::Commands<App>                         commands;
    webfront::UISubscription                        subscription;
    std::map<std::string, webfront::inspect::Watch> watches;      // used on the server thread only
    std::atomic<bool>                               held{false};  // test.hold answers once test.letGo cleared it
};
}  // namespace

int main(int argc, char** argv) {
    try {
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_TEST_FRONTEND>();
        // CTest shows the output of a failed case only: it then names every request and the status it was answered,
        // which is what tells a resource the page could not load from one the server refused (#542).
        [[maybe_unused]] const auto requestLog = webfront::testdriver::logRequests();
        if (argc != 3 && argc != 4)
            throw std::invalid_argument("Expected fixture directory, header output directory and optional --public or --part=<name>");
        const Mode mode = parseMode(argc == 4 ? std::string_view(argv[3]) : std::string_view{});
        Part       part(argv[1], mode);
        part.show();
        return part.finish(argv[2]) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
