// The driver of the browser tests: runs one wf command of the developer environment without interaction, in an
// application that may add its own pages and C++ callbacks. webfront-wf (WebFrontWf.cpp) is the plain driver; the
// browser integration test (JasmineTest.cpp) adds the callbacks its specs call and checks what C++ observed.
//
// Exit status: the command's own status; 64 usage; 70 bootstrap failure (archive missing, wrong integrity, incompatible
// protocol or API); 71 time limit of the driver; 72 the browser closed without a result.
#pragma once

#include "../unit/TemporaryDirectory.hpp"
#include "DriverSuite.hpp"
#include "TestFrontend.hpp"
#include <webfront/system/SafePath.hpp>
#include <webfront/WebFront.hpp>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace webfront::testdriver {

// Browser tests switch environment revisions at identical URLs. Never share WebView2's
// persistent HTTP/module cache or background browser process between those cases.
struct DriverWebView2Options {
    static frontend::WebView2Options get() {
#ifdef _MSC_VER
        char*                                             raw    = nullptr;
        std::size_t                                       length = 0;
        const auto                                        status = _dupenv_s(&raw, &length, "WEBFRONT_TEST_WEBVIEW2_PROFILE");
        const std::unique_ptr<char, decltype(&std::free)> profile(raw, std::free);
        if (status != 0)
            throw std::runtime_error("Cannot read the WebView2 test profile");
        return get(profile.get());
#else
        return get(std::getenv("WEBFRONT_TEST_WEBVIEW2_PROFILE"));
#endif
    }
    static frontend::WebView2Options get(const char* profile) {
        frontend::WebView2Options options;
        options.visible = diagnosticWindows();
        if (profile && *profile)
            options.userDataFolder = std::filesystem::path(profile);
        return options;
    }
};

// CEF's default profile is beside the executable, independent of the HTTP port. Each driver process/session
// uses its own profile so Chromium cannot forward an invocation into another test's browser singleton.
struct DriverCEFFrontend : frontend::CEFFrontend {
    static void initialize() {
        webfront::detail::ensureFrontendInitialized<frontend::CEFFrontend>();
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy = {}) {
#if defined(WEBFRONT_EMBED_CEF) && defined(__linux__)
        const auto* profile = std::getenv("WEBFRONT_TEST_CEF_PROFILE");
        if (!profile || !*profile)
            throw std::runtime_error("CEF test driver requires an isolated WEBFRONT_TEST_CEF_PROFILE");
        cef::detail::openWindow(frontend::detail::applicationUrl(address, port, page),
                                webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                                nullptr,
                                std::filesystem::path(profile));
#else
        frontend::CEFFrontend::open(address, port, page, policy);
#endif
    }
};

template <typename Frontend>
using DriverFrontend = std::conditional_t<std::is_same_v<Frontend, frontend::WebView2Frontend>,
                                          frontend::WebView2FrontendWithOptions<DriverWebView2Options>,
                                          std::conditional_t<std::is_same_v<Frontend, frontend::WKWebViewFrontend>,
                                                             QuietFrontend<Frontend>,
                                                             std::conditional_t<std::is_same_v<Frontend, frontend::CEFFrontend>, DriverCEFFrontend, Frontend>>>;
using namespace std::chrono_literals;

inline constexpr int usageStatus     = 64;
inline constexpr int bootstrapStatus = 70;
inline constexpr int timeoutStatus   = 71;
inline constexpr int noResultStatus  = 72;

struct Options {
    std::filesystem::path                project;
    std::optional<std::filesystem::path> environment;
    std::optional<std::string>           integrity;
    std::optional<std::filesystem::path> registryDirectory;
    std::string                          manifestDirectory;
    std::optional<std::filesystem::path> report;
    std::chrono::seconds                 timeout{600};
    std::vector<std::string>             command;
    std::optional<std::filesystem::path> suite;
    std::optional<std::filesystem::path> reportDirectory;
    std::string                          filter;
};

struct UsageError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

inline Options parse(int argc, char** argv) {
    Options                        options;
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& argument = arguments[i];
        const auto  value    = [&]() -> const std::string& {
            if (++i == arguments.size())
                throw UsageError(argument + " expects a value");
            return arguments[i];
        };
        if (argument == "--") {
            options.command.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i) + 1, arguments.end());
            break;
        }
        if (argument == "--project")
            options.project = value();
        else if (argument == "--environment")
            options.environment = value();
        else if (argument == "--integrity")
            options.integrity = value();
        else if (argument == "--registry-dir")
            options.registryDirectory = value();
        else if (argument == "--manifest-dir")
            options.manifestDirectory = value();
        else if (argument == "--suite")
            options.suite = value();
        else if (argument == "--report-dir")
            options.reportDirectory = value();
        else if (argument == "--filter")
            options.filter = value();
        else if (argument == "--report")
            options.report = value();
        else if (argument == "--timeout") {
            const auto& seconds     = value();
            long        parsed      = 0;
            const auto [end, error] = std::from_chars(seconds.data(), seconds.data() + seconds.size(), parsed);
            if (error != std::errc{} || end != seconds.data() + seconds.size() || parsed < 1)
                throw UsageError("--timeout expects whole seconds");
            options.timeout = std::chrono::seconds(parsed);
        } else if (argument.starts_with("--"))
            throw UsageError("Unknown option " + argument);
        else {
            options.command.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i), arguments.end());
            break;
        }
    }
    if (options.project.empty())
        throw UsageError("--project is required");
    if (!options.manifestDirectory.empty()) {
        try {
            fs::safeRelativePath(options.manifestDirectory);
        } catch (const std::invalid_argument&) {
            throw UsageError("--manifest-dir expects a relative '/'-separated directory without '..'");
        }
    }
    if (options.command.empty() && !options.suite)
        throw UsageError("A wf command is required");
    if (options.suite && (!options.command.empty() || options.report || !options.reportDirectory))
        throw UsageError("--suite requires --report-dir and replaces the command and --report");
    if (!options.suite && (options.reportDirectory || !options.filter.empty()))
        throw UsageError("--report-dir and --filter require --suite");
    if (options.environment.has_value() != options.integrity.has_value())
        throw UsageError("--environment and --integrity go together");
#if defined(WEBFRONT_BOOTSTRAP_ARCHIVE) && defined(WEBFRONT_BOOTSTRAP_INTEGRITY)
    if (!options.environment) {
        options.environment = WEBFRONT_BOOTSTRAP_ARCHIVE;
        options.integrity   = WEBFRONT_BOOTSTRAP_INTEGRITY;
    }
#endif
    if (!options.environment)
        throw UsageError("Pass --environment and --integrity: no bootstrap archive was provisioned at build time");
    return options;
}

// Where DriverFS finds what it serves: a filesystem provider is built from the project directory alone, and this process
// runs a single application.
struct Sources {
    std::filesystem::path                archive;
    std::optional<std::filesystem::path> registry;
};
inline Sources& sources() {
    static Sources instance;
    return instance;
}

// Real HTTP page: WebFront.js derives its WebSocket URL from location.host (srcdoc has no host).
inline constexpr std::string_view suitePage = R"HTML(<!doctype html><div id="preview"></div><script src="/WebFront.js"></script><script type="module">
                    Promise.resolve().then(async () => {
                        localStorage.clear();
                        sessionStorage.clear();
                        // No environment command uses IndexedDB today, but discard databases created by a spec when supported.
                        for (const database of await indexedDB.databases?.() ?? []) {
                            await new Promise((resolve, reject) => {
                                const request = indexedDB.deleteDatabase(database.name);
                                request.onsuccess = resolve;
                                request.onerror = () => reject(request.error);
                                request.onblocked = () => reject(new Error('A previous case retained an IndexedDB connection'));
                            });
                        }
                        return import('/webfront/developer/environment/' + parent.webfrontDriverCase.entry);
                    }).then(async runtime => {
                        await webFront.ready;
                        const {index, item, config} = parent.webfrontDriverCase;
                        let result;
                        await runtime.runNonInteractive(item.command, {bridge: webFront, config,
                            preview: document.getElementById('preview'), signal: new AbortController().signal,
                            write: () => {}, onExit: () => {}, reporter: async json => {
                                result = JSON.parse(json);
                                await webFront.cppFunction('wf.caseResult')(index, result.exitCode, json);
                            }});
                        if (!result) throw new Error('Suite command returned no report');
                        parent.postMessage({driverCase: index, status: result.exitCode}, location.origin);
                    }).catch(error => parent.postMessage({driverCase: parent.webfrontDriverCase.index,
                        error: String(error?.stack ?? error)}, location.origin));</script>)HTML";

/// Serves the environment archive and the optional local registry under /driver/, from outside the project.
class DriverFS {
public:
    explicit DriverFS(const std::filesystem::path&) {}

    std::optional<webfront::fs::File> open(std::filesystem::path path) const {
        const auto relative = path.relative_path().generic_string();
        if (relative == "driver/case.html") {
            static const auto words = [] {
                std::vector<std::uint64_t> packed((suitePage.size() + 7) / 8);
                for (std::size_t i = 0; i < suitePage.size(); ++i)
                    packed[i / 8] |= static_cast<std::uint64_t>(static_cast<unsigned char>(suitePage[i])) << ((7 - i % 8) * 8);
                return packed;
            }();
            return fs::File{std::span<const std::uint64_t>{words}, suitePage.size()};
        }
        if (relative == "driver/environment.tgz")
            return read(sources().archive);
        // The starters the environment pins sit next to its archive: plain file names only.
        constexpr std::string_view driverPrefix = "driver/";
        if (relative.starts_with(driverPrefix)) {
            const std::filesystem::path name = relative.substr(driverPrefix.size());
            if (name.extension() == ".tgz" && name == name.filename() && name.string().find("..") == std::string::npos)
                return read(sources().archive.parent_path() / name);
        }
        constexpr std::string_view prefix = "driver/registry/";
        if (sources().registry && relative.starts_with(prefix)) {
            try {
                return read(webfront::developer::projectPath(*sources().registry, std::string_view(relative).substr(prefix.size())));
            } catch (const std::exception&) {}
        }
        return {};
    }

private:
    static std::optional<webfront::fs::File> read(const std::filesystem::path& file) {
        std::ifstream stream(file, std::ios::binary);
        if (stream && std::filesystem::is_regular_file(file))
            return webfront::fs::File{std::move(stream)};
        return {};
    }
};

// What the page reports: the command's result, or the reason the environment could not start.
class Outcome {
public:
    void result(int exitCode, std::string json) {
        finish([&] {
            status     = exitCode;
            resultJson = std::move(json);
            hasResult  = true;
        });
    }
    void bootstrapFailed(std::string message) {
        finish([&] {
            status  = bootstrapStatus;
            failure = std::move(message);
        });
    }
    void timedOut() {
        finish([&] {
            status  = timeoutStatus;
            failure = "The time limit of the driver elapsed";
        });
    }
    bool finished() const {
        std::scoped_lock lock{mutex};
        return done;
    }
    template <typename Function>
    void inspect(Function&& function) const {
        std::scoped_lock lock{mutex};
        function(done, status, hasResult, resultJson, failure);
    }
    bool wait(std::chrono::seconds limit) {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, limit, [this] {
            return done;
        });
    }

private:
    template <typename Function>
    void finish(Function&& function) {
        {
            std::scoped_lock lock{mutex};
            if (done)
                return;  // the first outcome wins
            function();
            done = true;
        }
        changed.notify_all();
    }

    mutable std::mutex      mutex;
    std::condition_variable changed;
    bool                    done      = false;
    bool                    hasResult = false;
    int                     status    = noResultStatus;
    std::string             resultJson;
    std::string             failure;
};

// Waits for the environment, runs the command and reports to C++; the page closes itself when it is done.
inline constexpr auto driverScript = R"JS(
(async () => {
    const report = (name, ...args) => webFront.cppFunction(name)(...args);
    const stop = () => window.close();
    try {
        await webFront.ready;
        // Observe the bootstrap status rather than polling the DOM every 50 ms.
        await new Promise((resolve, reject) => {
            const observer = new MutationObserver(check);
            function check() {
                const text = document.getElementById('status')?.textContent ?? '';
                if (text.startsWith('Bootstrap failed:')) { observer.disconnect(); reject(new Error(text)); }
                else if (text.startsWith('Developer environment ready.')) { observer.disconnect(); resolve(); }
            }
            observer.observe(document.documentElement, {subtree: true, childList: true, characterData: true});
            check();
        });
        const config = JSON.parse(await report('webfront.developer.config'));
        const environment = await (await fetch('/webfront/developer/environment/environment.json')).json();
        const runtime = await import(`/webfront/developer/environment/${environment.entry}`);
        if (typeof runtime.runNonInteractive !== 'function')
            throw Object.assign(new Error('Bootstrap failed: this developer environment has no non-interactive mode'), {bootstrap: true});
        config.starters = Object.fromEntries(Object.entries(environment.starters ?? {}).map(([name, value]) =>
            [name, {url: new URL(value.archive, new URL(config.environment, location.href)).href, integrity: value.integrity}]));
        const suite = JSON.parse(await report('wf.suite'));
        if (suite) {
            let failed = false;
            // One native browser, one fresh realm per command: imports, timers, Workers, listeners and frames
            // belong to that realm. Only the explicit project and immutable HTTP resources survive between cases.
            for (let index = 0; index < suite.length; ++index) {
                const item = suite[index];
                await report('wf.caseStarted', index);
                const frame = document.createElement('iframe');
                frame.style.cssText = 'position:fixed;left:-10000px;top:0;width:1024px;height:768px';
                const finished = new Promise((resolve, reject) => {
                    const receive = event => {
                        if (event.source !== frame.contentWindow || event.data?.driverCase !== index) return;
                        removeEventListener('message', receive);
                        event.data.error ? reject(new Error(event.data.error)) : resolve(event.data.status);
                    };
                    addEventListener('message', receive);
                });
                window.webfrontDriverCase = {index, item, config, entry: environment.entry};
                frame.src = '/driver/case.html?case=' + index;
                document.body.append(frame);
                try { failed = (await finished !== item.expect) || failed; }
                finally { frame.remove(); delete window.webfrontDriverCase; }
            }
            await report('wf.result', failed ? 1 : 0, JSON.stringify({exitCode: failed ? 1 : 0, output: 'Suite completed'}));
        } else {
            const argv = JSON.parse(await report('wf.arguments'));
            await runtime.runNonInteractive(argv, {bridge: webFront, config, preview: document.getElementById('preview'),
                signal: new AbortController().signal, write: () => {}, onExit: () => {},
                reporter: json => report('wf.result', JSON.parse(json).exitCode, json)});
        }
    } catch (error) {
        await report('wf.bootstrapFailed', String(error?.message ?? error)).catch(() => {});
    } finally { stop(); }
})();
)JS";

// Owns the connected UI and remembers a close requested before the handshake. Connection and
// watchdog callbacks share one lock, so neither ordering can lose the close or launch a late command.
template <typename UI>
class DriverBrowser {
public:
    void connected(UI ui) {
        std::scoped_lock lock{mutex};
        if (browser)
            return;  // Commands and wf test create their own connected frames; inject only the native page.
        browser.emplace(std::move(ui));
        browser->addScript(closing ? "window.close()" : driverScript);
    }
    void close() {
        std::scoped_lock lock{mutex};
        if (std::exchange(closing, true))
            return;
        if (browser)
            browser->addScript("window.close()");
    }

private:
    std::mutex        mutex;
    std::optional<UI> browser;
    bool              closing = false;
};

inline std::string jsonArray(const std::vector<std::string>& items) {
    std::string json = "[";
    for (const auto& item : items)
        json += (json.size() > 1 ? "," : "") + webfront::json::quote(item);
    return json + "]";
}

inline void writeReport(const Options& options, const std::string& json) {
    if (!options.report)
        return;
    std::ofstream file(*options.report, std::ios::binary | std::ios::trunc);
    file << json << '\n';
    if (!file)
        throw std::runtime_error("Cannot write the report " + options.report->string());
}

// Prints what the command wrote: the "output" member of the result.
inline void printOutput(const std::string& json) {
    webfront::json::Reader reader{json};
    reader.object([&](const std::string& key) {
        if (key == "output")
            std::cout << reader.string() << std::endl;
        else
            reader.skipValue();
    });
}

/// An application that adds nothing to the driver: no page, no callback, the status of the command.
struct PlainSetup {
    struct Pages {
        explicit Pages(const std::filesystem::path&) {}
        std::optional<webfront::fs::File> open(std::filesystem::path) const {
            return {};
        }
    };
    template <typename App>
    void configure(App&) {}
    int  finish(int status) {
        return status;
    }
};

/// Runs `options.command` in an application made of the driver and of `Setup`: `Setup::Pages` is a filesystem provider
/// built from the project directory, `configure(app)` registers callbacks before the page opens, and `finish(status)`
/// returns the process status once the application is shut down (the command's status, unless the specs also demand
/// what C++ observed).
template <typename Frontend, typename Setup = PlainSetup>
int run(const Options& options, Setup&& setup = {}) {
    using App = webfront::BasicDeveloperApp<DriverFrontend<Frontend>, DriverFS, typename std::remove_reference_t<Setup>::Pages>;
    if (!std::filesystem::is_regular_file(*options.environment)) {
        std::cerr << "Bootstrap environment archive not found: " << *options.environment << '\n';
        return bootstrapStatus;
    }
    if (!std::filesystem::is_directory(options.project)) {
        std::cerr << "Project directory not found: " << options.project << '\n';
        return usageStatus;
    }
    sources() = {std::filesystem::canonical(*options.environment), options.registryDirectory};
    if (sources().registry)
        sources().registry = std::filesystem::canonical(*sources().registry);

    const auto cases = options.suite ? parseSuite(readSuite(*options.suite), options.filter) : std::vector<SuiteCase>{};
    if (options.reportDirectory) {
        std::filesystem::create_directories(*options.reportDirectory);
        for (const auto& item : cases)
            std::filesystem::remove(*options.reportDirectory / (item.name + ".json"));
    }
    std::string suiteJson = "null";
    if (!cases.empty()) {
        suiteJson = "[";
        for (const auto& item : cases)
            suiteJson += (suiteJson.size() > 1 ? "," : "") + std::string("{\"name\":") + json::quote(item.name) + ",\"command\":" + jsonArray(item.command)
                         + ",\"expect\":" + std::to_string(item.expected) + "}";
        suiteJson += "]";
        if (suiteJson.size() > 60000)
            throw UsageError("Suite arguments exceed the bridge string budget (60000 bytes)");
    }
    std::size_t completed  = 0;
    bool        caseFailed = false;
    const auto  started    = std::chrono::steady_clock::now();
    const auto  phase      = [&](std::string_view name) {
        std::cout << "driver " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() << "ms " << name << std::endl;
    };
    DriverDeadline deadlines{options.timeout};
    Outcome        outcome;
    App            app{options.project, {.port = "0"}};
    app.cppFunction("wf.suite", [suiteJson] {
        return suiteJson;
    });
    app.cppFunction("wf.caseStarted", [&](double index) {
        if (index != static_cast<double>(completed) || completed >= cases.size())
            throw std::runtime_error("Unexpected suite start index");
        deadlines.beginCase(cases[completed].timeout);
        phase("begin " + cases[completed].name);
    });
    app.cppFunction("wf.caseResult", [&](double index, double exitCode, std::string json) {
        if (index != static_cast<double>(completed) || completed >= cases.size())
            throw std::runtime_error("Unexpected suite result index");
        deadlines.endCase();
        const auto& item = cases[completed];
        printOutput(json);
        Options reportOptions;
        reportOptions.report = *options.reportDirectory / (item.name + ".json");
        writeReport(reportOptions, json);
        if (exitCode != item.expected)
            std::cerr << "suite case " << item.name << ": expected status " << item.expected << ", got " << exitCode << std::endl;
        caseFailed = (exitCode != item.expected) || caseFailed;
        phase("end " + item.name);
        ++completed;
    });
    app.cppFunction("wf.arguments", [arguments = jsonArray(options.command)] {
        return arguments;
    });
    app.cppFunction("wf.result", [&outcome](double exitCode, std::string json) {
        outcome.result(static_cast<int>(exitCode), std::move(json));
    });
    app.cppFunction("wf.bootstrapFailed", [&outcome](std::string message) {
        outcome.bootstrapFailed(std::move(message));
    });
    app.cppFunction("wf.progress", [](std::string message) {
        std::cout << "wf spec: " << message << std::endl;
    });
    setup.configure(app);
    app.bootstrap("developer",
                  {.environment       = "/driver/environment.tgz",
                   .integrity         = *options.integrity,
                   .registry          = options.registryDirectory ? std::optional<std::string>{"/driver/registry/"} : std::nullopt,
                   .manifestDirectory = options.manifestDirectory});

    DriverBrowser<typename App::UI> browser;
    auto                            subscription = app.subscribeUIStarted([&](typename App::UI ui) {
        browser.connected(ui);
    });
    // The time limit: ask the page to close, then end the process if the browser does not follow.
    std::jthread watchdog([&](std::stop_token stop) {
        std::mutex                  mutex;
        std::condition_variable_any wake;
        std::unique_lock            lock{mutex};
        if (!deadlines.expired(stop))
            return;
        outcome.timedOut();
        try {
            browser.close();
        } catch (const std::exception&) {}  // the bridge is gone: the forced exit below still applies
        if (!stop.stop_requested()
            && !wake.wait_for(lock,
                              stop,
                              10s,
                              [] {
                                  return false;
                              })
            && !stop.stop_requested()) {
            std::cerr << "The browser did not close after the time limit of " << options.timeout.count() << " s\n";
            std::cerr.flush();
            std::_Exit(timeoutStatus);
        }
    });
    phase("begin browser");
    app.show();
    phase("end browser / begin shutdown");
    watchdog.request_stop();
    app.shutdown();
    phase("end shutdown");

    int         status    = noResultStatus;
    bool        hasResult = false;
    std::string json, failure;
    outcome.inspect([&](bool, int code, bool result, const std::string& resultJson, const std::string& reason) {
        status    = code;
        hasResult = result;
        json      = resultJson;
        failure   = reason;
    });
    if (hasResult) {
        printOutput(json);
        writeReport(options, json);
    } else if (status == bootstrapStatus || status == timeoutStatus)
        std::cerr << failure << '\n';
    else
        std::cerr << "The browser closed before the command reported a result\n";
    if (!cases.empty() && status == 0 && (caseFailed || completed != cases.size()))
        status = 1;
    return setup.finish(status);
}

/// The entry point of a driver: CEF child processes dispatch first, then the arguments are read and the command runs.
template <typename Frontend, typename Setup = PlainSetup>
int main(int argc, char** argv, std::string_view program, Setup&& setup = {}) {
    try {
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<Frontend>();
#if defined(WEBFRONT_EMBED_CEF) && defined(__linux__)
        std::optional<webfront::test::TemporaryDirectory> temporaryProfile;
        if constexpr (std::is_same_v<Frontend, frontend::CEFFrontend>) {
            if (!std::getenv("WEBFRONT_TEST_CEF_PROFILE")) {
                temporaryProfile.emplace();
                ::setenv("WEBFRONT_TEST_CEF_PROFILE", temporaryProfile->path().c_str(), 1);
            }
        }
#endif
        return run<Frontend>(parse(argc, argv), std::forward<Setup>(setup));
    } catch (const UsageError& error) {
        std::cerr << program << ": " << error.what() << "\nUsage: " << program << " --project <dir> [--environment <archive> --integrity <sha512-...>] "
                  << "[--registry-dir <dir>] [--report <file>] [--timeout <seconds>] [--suite <json> --report-dir <dir> [--filter <name>]] [--] <wf command> "
                     "[arguments...]\n";
        return usageStatus;
    } catch (const std::exception& error) {
        std::cerr << program << ": " << error.what() << '\n';
        return bootstrapStatus;
    }
}
}  // namespace webfront::testdriver
