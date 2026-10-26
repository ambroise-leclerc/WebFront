// The driver of the browser tests: runs one wf command of the developer environment without interaction, in an
// application that may add its own pages and C++ callbacks. webfront-wf (WebFrontWf.cpp) is the plain driver; the
// browser integration test (JasmineTest.cpp) adds the callbacks its specs call and checks what C++ observed.
//
// Exit status: the command's own status; 64 usage; 70 bootstrap failure (archive missing, wrong integrity, incompatible
// protocol or API); 71 time limit of the driver; 72 the browser closed without a result.
#pragma once

#include <webfront/WebFront.hpp>
#include <webfront/system/SafePath.hpp>


#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <memory>
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
        char* raw = nullptr;
        std::size_t length = 0;
        const auto status = _dupenv_s(&raw, &length, "WEBFRONT_TEST_WEBVIEW2_PROFILE");
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
        if (profile && *profile)
            options.userDataFolder = std::filesystem::path(profile);
        return options;
    }
};

template <typename Frontend>
using DriverFrontend = std::conditional_t<std::is_same_v<Frontend, frontend::WebView2Frontend>,
                                          frontend::WebView2FrontendWithOptions<DriverWebView2Options>, Frontend>;
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
    if (options.command.empty())
        throw UsageError("A wf command is required");
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

/// Serves the environment archive and the optional local registry under /driver/, from outside the project.
class DriverFS {
public:
    explicit DriverFS(const std::filesystem::path&) {}

    std::optional<webfront::fs::File> open(std::filesystem::path path) const {
        const auto relative = path.relative_path().generic_string();
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
        for (;;) {
            const text = document.getElementById('status')?.textContent ?? '';
            if (text.startsWith('Bootstrap failed:')) throw Object.assign(new Error(text), {bootstrap: true});
            if (text.startsWith('Developer environment ready.')) break;
            await new Promise(resolve => setTimeout(resolve, 50));
        }
        const config = JSON.parse(await report('webfront.developer.config'));
        const environment = await (await fetch('/webfront/developer/environment/environment.json')).json();
        const runtime = await import(`/webfront/developer/environment/${environment.entry}`);
        if (typeof runtime.runNonInteractive !== 'function')
            throw Object.assign(new Error('Bootstrap failed: this developer environment has no non-interactive mode'), {bootstrap: true});
        config.starters = Object.fromEntries(Object.entries(environment.starters ?? {}).map(([name, value]) =>
            [name, {url: new URL(value.archive, new URL(config.environment, location.href)).href, integrity: value.integrity}]));
        const argv = JSON.parse(await report('wf.arguments'));
        await runtime.runNonInteractive(argv, {bridge: webFront, config, preview: document.getElementById('preview'),
            signal: new AbortController().signal, write: () => {}, onExit: () => {},
            reporter: json => report('wf.result', JSON.parse(json).exitCode, json)});
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
    std::mutex mutex;
    std::optional<UI> browser;
    bool closing = false;
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
    int finish(int status) {
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

    Outcome outcome;
    App     app{options.project, {}};
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
                  {.environment = "/driver/environment.tgz",
                   .integrity   = *options.integrity,
                   .registry    = options.registryDirectory ? std::optional<std::string>{"/driver/registry/"} : std::nullopt,
                   .manifestDirectory = options.manifestDirectory});

    DriverBrowser<typename App::UI> browser;
    auto subscription = app.subscribeUIStarted([&](typename App::UI ui) {
        browser.connected(ui);
    });
    // The time limit: ask the page to close, then end the process if the browser does not follow.
    std::jthread watchdog([&](std::stop_token stop) {
        std::mutex                  mutex;
        std::condition_variable_any wake;
        std::unique_lock            lock{mutex};
        if (wake.wait_for(lock,
                          stop,
                          options.timeout,
                          [] {
                              return false;
                          })
            || stop.stop_requested())
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
    app.show();
    watchdog.request_stop();
    app.shutdown();

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
    return setup.finish(status);
}

/// The entry point of a driver: CEF child processes dispatch first, then the arguments are read and the command runs.
template <typename Frontend, typename Setup = PlainSetup>
int main(int argc, char** argv, std::string_view program, Setup&& setup = {}) {
    try {
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<Frontend>();
        return run<Frontend>(parse(argc, argv), std::forward<Setup>(setup));
    } catch (const UsageError& error) {
        std::cerr << program << ": " << error.what() << "\nUsage: " << program
                  << " --project <dir> [--environment <archive> --integrity <sha512-...>] "
                  << "[--registry-dir <dir>] [--report <file>] [--timeout <seconds>] [--] <wf command> [arguments...]\n";
        return usageStatus;
    } catch (const std::exception& error) {
        std::cerr << program << ": " << error.what() << '\n';
        return bootstrapStatus;
    }
}
}  // namespace webfront::testdriver
