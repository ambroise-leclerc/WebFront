// webfront-wf: runs one wf command of the browser developer environment without interaction and exits with its status.
//
//   webfront-wf --project <dir> [--environment <archive> --integrity <sha512-...>] [--registry-dir <dir>]
//               [--report <file>] [--timeout <seconds>] [--] <wf command> [arguments...]
//
// The environment defaults to the provisioned bootstrap archive (cmake/WebFrontBootstrap.cmake); the project is the
// directory wf reads and writes. Exit status: the command's own status; 64 usage; 70 bootstrap failure (archive missing,
// wrong integrity, incompatible protocol or API); 71 time limit of the driver; 72 the browser closed without a result.
#include <webfront/WebFront.hpp>

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifndef WEBFRONT_DRIVER_FRONTEND
    #error "WEBFRONT_DRIVER_FRONTEND must name the browser frontend"
#endif

namespace {
using namespace std::chrono_literals;

constexpr int usageStatus     = 64;
constexpr int bootstrapStatus = 70;
constexpr int timeoutStatus   = 71;
constexpr int noResultStatus  = 72;

struct Options {
    std::filesystem::path                project;
    std::optional<std::filesystem::path> environment;
    std::optional<std::string>           integrity;
    std::optional<std::filesystem::path> registryDirectory;
    std::optional<std::filesystem::path> report;
    std::chrono::seconds                 timeout{600};
    std::vector<std::string>             command;
};

struct UsageError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

Options parse(int argc, char** argv) {
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
Sources& sources() {
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
constexpr auto driverScript = R"JS(
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
        const runtime = await import('/webfront/developer/environment/runtime.mjs');
        if (typeof runtime.runNonInteractive !== 'function')
            throw Object.assign(new Error('Bootstrap failed: this developer environment has no non-interactive mode'), {bootstrap: true});
        const config = JSON.parse(await report('webfront.developer.config'));
        const environment = await (await fetch('/webfront/developer/environment/environment.json')).json();
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

std::string jsonArray(const std::vector<std::string>& items) {
    std::string json = "[";
    for (const auto& item : items)
        json += (json.size() > 1 ? "," : "") + webfront::json::quote(item);
    return json + "]";
}

void writeReport(const Options& options, const std::string& json) {
    if (!options.report)
        return;
    std::ofstream file(*options.report, std::ios::binary | std::ios::trunc);
    file << json << '\n';
    if (!file)
        throw std::runtime_error("Cannot write the report " + options.report->string());
}

// Prints what the command wrote: the "output" member of the result.
void printOutput(const std::string& json) {
    webfront::json::Reader reader{json};
    reader.object([&](const std::string& key) {
        if (key == "output")
            std::cout << reader.string() << std::endl;
        else
            reader.skipValue();
    });
}

int run(const Options& options) {
    using App = webfront::BasicDeveloperApp<WEBFRONT_DRIVER_FRONTEND, DriverFS>;
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
    app.bootstrap("developer",
                  {.environment = "/driver/environment.tgz",
                   .integrity   = *options.integrity,
                   .registry    = options.registryDirectory ? std::optional<std::string>{"/driver/registry/"} : std::nullopt});

    std::mutex             uiMutex;
    std::optional<App::UI> browser;
    auto                   subscription = app.subscribeUIStarted([&](App::UI ui) {
        ui.addScript(driverScript);
        std::scoped_lock lock{uiMutex};
        browser.emplace(ui);
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
        {
            std::scoped_lock ui{uiMutex};
            if (browser)
                browser->addScript("window.close()");
        }
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
    return status;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        // CEF child processes must dispatch before interpreting application arguments or creating files.
        webfront::detail::ensureFrontendInitialized<WEBFRONT_DRIVER_FRONTEND>();
        return run(parse(argc, argv));
    } catch (const UsageError& error) {
        std::cerr << "webfront-wf: " << error.what() << "\nUsage: webfront-wf --project <dir> [--environment <archive> --integrity <sha512-...>] "
                  << "[--registry-dir <dir>] [--report <file>] [--timeout <seconds>] [--] <wf command> [arguments...]\n";
        return usageStatus;
    } catch (const std::exception& error) {
        std::cerr << "webfront-wf: " << error.what() << '\n';
        return bootstrapStatus;
    }
}
