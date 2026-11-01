/// @brief Opt-in Jasmine instrumentation of a consumer's real page, enabled by a CTest configuration.
#pragma once

#include "../utils/Json.hpp"
#include "../WebFront.hpp"
#include "Capture.hpp"
#include "Host.hpp"

#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace webfront::testing {

namespace detail {
inline constexpr std::size_t jasmineFileLimit = 4 * 1024 * 1024;

inline std::string readJasmineFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read Jasmine file: " + path.string());
    std::string text;
    char        buffer[16384];
    while (file.read(buffer, sizeof buffer) || file.gcount()) {
        text.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (text.size() > jasmineFileLimit)
            throw std::runtime_error("Jasmine file exceeds 4 MiB");
    }
    if (!file.eof())
        throw std::runtime_error("Cannot finish reading Jasmine file");
    return text;
}

struct JasmineConfiguration {
    std::vector<std::string> files;
    std::filesystem::path    report;
    std::filesystem::path    captures;  // Optional: where screenshots and recordings go.
};

inline JasmineConfiguration jasmineConfiguration(std::string_view text) {
    JasmineConfiguration config;
    json::Reader         reader{text};
    bool                 files = false, report = false, captures = false;
    reader.object([&](const std::string& key) {
        if (key == "files" && !std::exchange(files, true))
            reader.array([&] {
                if (config.files.size() >= 101)
                    throw std::invalid_argument("At most 100 Jasmine spec files are supported");
                config.files.push_back(reader.string());
            });
        else if (key == "report" && !std::exchange(report, true))
            config.report = reader.string();
        else if (key == "captures" && !std::exchange(captures, true))
            config.captures = reader.string();
        else
            throw std::invalid_argument("Unknown or duplicate Jasmine configuration field");
    });
    reader.end();
    if (config.files.size() < 2 || config.report.empty())
        throw std::invalid_argument("Jasmine needs its runtime, specs and report path");
    return config;
}

/// The value of an environment variable the test runner sets, if it is set and not empty.
inline std::optional<std::string> jasmineEnvironment(const char* name) {
#ifdef _MSC_VER
    char*       raw  = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0)
        throw std::runtime_error(std::string("Cannot read ") + name);
    std::unique_ptr<char, decltype(&std::free)> owned(raw, std::free);
    if (raw && *raw)
        return std::string(raw);
#else
    if (const auto* raw = std::getenv(name); raw && *raw)
        return std::string(raw);
#endif
    return std::nullopt;
}

inline std::optional<std::string> jasmineConfigurationPath() {
    return jasmineEnvironment("WEBFRONT_JASMINE_CONFIG");
}

/// Bytes per bridge read, shared by the page script (injected as CHUNK) and JasmineSession::read.
inline constexpr std::size_t jasmineChunkSize = 49152;

inline constexpr std::string_view jasmineScriptBody = R"JS(
void (async () => {
    const call = (name, ...args) => webFront.cppFunction('webfront.testing.' + name)(...args);
    // Screenshots and image sequences of the web content, written to the run's capture directory. The C++ side starts the
    // work on its own thread and answers at once (the server thread must keep serving the page), so the page polls.
    const settle = async name => {
        for (;;) {
            const result = await call(name);
            if (result >= 0) return result;
            await new Promise(resolve => setTimeout(resolve, 20));
        }
    };
    // The methods do not use `this`, so `const {record} = webFrontCapture` works. A recording whose page is replaced is
    // stopped by the C++ side.
    const screenshot = async name => { await call('screenshotStart', name); return settle('screenshotPoll'); };
    const startRecording = (name, framesPerSecond = 10) => call('recordStart', name, framesPerSecond);
    const stopRecording = async () => { await call('recordStop'); return settle('recordPoll'); };
    const record = async (name, {seconds = 3, framesPerSecond = 10} = {}) => {
        await startRecording(name, framesPerSecond);
        await new Promise(resolve => setTimeout(resolve, seconds * 1000));
        return stopRecording();
    };
    globalThis.webFrontCapture = {screenshot, startRecording, stopRecording, record};
    // Native capabilities the test's owner granted (PROGRAMS in webfront_add_jasmine_test): authorized programs run without a
    // shell, and private temporary spaces. Without a grant every call rejects. Bytes cross the bridge in bounded chunks.
    const hostChunk = CHUNK, HOST_FILE_LIMIT = 64 * 1024 * 1024;
    const bytesOf = data => typeof data === 'string' ? new TextEncoder().encode(data) : new Uint8Array(data);
    const readAll = async fetchChunk => {
        const parts = [];
        for (let offset = 0; ; ) {
            const part = await fetchChunk(offset);
            parts.push(part);
            offset += part.length;
            if (offset > HOST_FILE_LIMIT) throw new RangeError('A file or output exceeds the 64 MiB limit');
            if (part.length < hostChunk) break;
        }
        return new Blob(parts).arrayBuffer().then(buffer => new Uint8Array(buffer));
    };
    const host = {
        programs: async () => JSON.parse(await call('hostPrograms')),
        makeTemp: () => call('hostMakeTemp'),
        removeTemp: path => call('hostRemoveTemp', path),
        withTemp: async body => {
            const path = await call('hostMakeTemp');
            try { return await body(path); }
            finally { await call('hostRemoveTemp', path); }
        },
        writeFile: async (path, data) => {
            const bytes = bytesOf(data);
            await call('hostWrite', path, 0, bytes.subarray(0, 0));
            for (let offset = 0; offset < bytes.length; offset += hostChunk)
                await call('hostWrite', path, offset, bytes.subarray(offset, offset + hostChunk));
        },
        readFile: path => readAll(offset => call('hostRead', path, offset)),
        readText: async path => new TextDecoder().decode(await host.readFile(path)),
        run: async (options = {}) => {
            const known = ['program', 'args', 'cwd', 'env', 'inheritEnv', 'timeoutMs', 'maxOutputBytes', 'signal'];
            // There is no shell option to ask for: an unknown one is a mistake, not something to ignore.
            for (const key of Object.keys(options)) if (!known.includes(key)) throw new TypeError('Unknown run option: ' + key);
            const {program, args = [], cwd, env, inheritEnv, timeoutMs, maxOutputBytes, signal} = options;
            const request = {program, args: args.map(String)};
            for (const [key, value] of Object.entries({cwd, env, inheritEnv, timeoutMs, maxOutputBytes}))
                if (value !== undefined) request[key] = value;
            signal?.throwIfAborted();
            // The listener exists before the start call: an abort that lands while the program starts is not lost.
            let id;
            const abort = () => { if (id !== undefined) call('hostRunCancel', id).catch(() => {}); };
            signal?.addEventListener('abort', abort, {once: true});
            try {
                id = await call('hostRunStart', JSON.stringify(request));
                if (signal?.aborted) abort();
                let summary = '';
                while (!(summary = await call('hostRunPoll', id))) await new Promise(resolve => setTimeout(resolve, 10));
                const result = JSON.parse(summary);
                const text = async stream => new TextDecoder().decode(await readAll(offset => call('hostRunRead', id, stream, offset)));
                const {stdoutBytes, stderrBytes, ...rest} = result;
                return {...rest, stdout: await text(1), stderr: await text(2)};
            } finally {
                signal?.removeEventListener('abort', abort);
                if (id !== undefined) await call('hostRunRelease', id);
            }
        },
    };
    globalThis.webFrontHost = host;
    const load = async index => {
        const parts = [];
        for (let offset = 0; ; offset += CHUNK) {
            const part = await call('read', index, offset);
            parts.push(part);
            if (part.length < CHUNK) break;
        }
        const script = document.createElement('script');
        script.textContent = await new Blob(parts).text();
        // A classic script reports a syntax error or a top-level throw as a window error, not as an exception.
        let failure;
        const capture = event => { failure ??= event.error ?? new Error(event.message); event.preventDefault(); };
        addEventListener('error', capture);
        try { document.head.append(script); }
        finally { removeEventListener('error', capture); script.remove(); }
        if (failure) throw new Error('Jasmine file ' + index + ' failed to load: ' + String(failure?.message ?? failure));
    };
    let reporting = false;
    const report = async result => {
        if (reporting) return;
        reporting = true;
        try {
            // The bridge string length is 16 bits of UTF-8 bytes: shed failure messages until the report fits.
            let text = JSON.stringify(result);
            while (new TextEncoder().encode(text).length > 60000 && result.failures?.length) {
                result.failures.pop();
                text = JSON.stringify(result);
            }
            await call('result', text);
        }
        catch (error) { console.error('Jasmine reporting failed', error); }
        finally { window.close(); }
    };
    try {
        // Handshake is not DOMContentLoaded: the application's ordinary load handlers run first.
        await webFront.ready;
        if (document.readyState !== 'complete') await new Promise(resolve => addEventListener('load', resolve, {once:true}));
        const count = await call('count');
        await load(0);
        if (!globalThis.jasmine) {
            const core = jasmineRequire.core(jasmineRequire);
            globalThis.jasmine = core;
            Object.assign(globalThis, jasmineRequire.interface(core, core.getEnv()));
        }
        const env = jasmine.getEnv();
        env.configure({random:false});
        let total = 0, failed = 0;
        const failures = [];
        const record = result => {
            for (const item of result.failedExpectations ?? [])
                if (failures.length < 50) failures.push(String(item.message).slice(0, 512));
        };
        env.addReporter({
            specDone(result) { if (result.status === 'passed' || result.status === 'failed') ++total; if (result.status === 'failed') ++failed; record(result); },
            suiteDone: record,
            jasmineDone(info) {
                record(info);
                return report({format:1, status: info.overallStatus === 'passed' && total > 0 && failed === 0 && !failures.length ? 'passed' : 'failed',
                    total, failed, failures, incomplete:String(info.incompleteReason ?? '').slice(0,1024)});
            }
        });
        for (let index = 1; index < count; ++index) await load(index);
        await env.execute();
    } catch (error) {
        await report({format:1, status:'error', error:(String(error?.message ?? error) + '\n' + String(error?.stack ?? '')).slice(0,8192)});
    }
})();
)JS";

/// The page script: the shared chunk size followed by the body that uses it.
inline std::string jasmineScript() {
    return "const CHUNK = " + std::to_string(jasmineChunkSize) + ";\n" + std::string(jasmineScriptBody);
}

/// The state shared by the bridge callbacks of one test run. The caller is the application's UI, compared by its
/// equality; any equality-comparable value (a bare WebLinkId by default) tests it without an application.
template <std::equality_comparable Caller = WebLinkId>
class JasmineSession {
public:
    static constexpr std::size_t chunkSize = jasmineChunkSize;

    JasmineSession(std::vector<std::string> contents, std::filesystem::path reportPath, std::shared_ptr<CaptureStore> captureStore = nullptr)
        : files(std::move(contents)), report(std::move(reportPath)), store(std::move(captureStore)) {}

    /// The newest page is the one under test (a reload replaces the previous connection). Returns false once finished.
    bool attach(const Caller& caller) {
        std::scoped_lock lock{mutex};
        if (completed)
            return false;
        if (browser && *browser != caller) {
            std::cerr << "Jasmine: a new page replaced the instrumented one\n";
            if (store)
                store->abandon();  // The replaced page can no longer collect its recording.
        }
        browser = caller;
        return true;
    }

    [[nodiscard]] double count(const Caller& caller) {
        std::scoped_lock lock{mutex};
        authorize(caller);
        return static_cast<double>(files.size());
    }

    [[nodiscard]] std::vector<std::uint8_t> read(const Caller& caller, double index, double offset) {
        std::scoped_lock lock{mutex};
        authorize(caller);
        if (!std::isfinite(index) || index < 0 || index >= static_cast<double>(files.size()) || std::floor(index) != index)
            throw std::invalid_argument("Invalid Jasmine file index");
        const auto& file = files[static_cast<std::size_t>(index)];
        if (!std::isfinite(offset) || offset < 0 || offset > static_cast<double>(file.size()) || std::floor(offset) != offset)
            throw std::invalid_argument("Invalid Jasmine file offset");
        const auto part = std::string_view(file).substr(static_cast<std::size_t>(offset), chunkSize);
        return std::vector<std::uint8_t>(part.begin(), part.end());
    }

    /// Starts writing `<captures>/<name>.png` on a worker thread and returns at once.
    void screenshotStart(const Caller& caller, const std::string& name) {
        captures(caller).beginScreenshot(name);
    }

    /// The size in bytes of the finished screenshot, or -1 while it is still being taken.
    [[nodiscard]] double screenshotPoll(const Caller& caller) {
        const auto size = captures(caller).pollScreenshot();
        return size ? static_cast<double>(*size) : -1.0;
    }

    void recordStart(const Caller& caller, const std::string& name, double framesPerSecond) {
        captures(caller).startRecording(name, framesPerSecond);
    }

    /// Asks the recording to stop and returns at once.
    void recordStop(const Caller& caller) {
        captures(caller).requestStop();
    }

    /// The number of images the recording wrote once it has ended, or -1 while it is still ending.
    [[nodiscard]] double recordPoll(const Caller& caller) {
        const auto frames = captures(caller).pollRecording();
        return frames ? static_cast<double>(*frames) : -1.0;
    }

    void result(const Caller& caller, const std::string& text) {
        std::scoped_lock lock{mutex};
        authorize(caller);
        if (completed)
            throw std::logic_error("Jasmine already completed");
        std::string  outcome;
        bool         seen = false;
        json::Reader reader{text};
        reader.object([&](const std::string& key) {
            if (key == "status") {
                if (std::exchange(seen, true))
                    throw std::invalid_argument("Duplicate Jasmine status");
                outcome = reader.string();
            } else
                reader.skipValue();
        });
        reader.end();
        if (outcome != "passed" && outcome != "failed" && outcome != "error")
            throw std::invalid_argument("Invalid Jasmine result");
        std::ofstream out(report, std::ios::binary | std::ios::trunc);
        out << text << '\n';
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write Jasmine report");
        status    = outcome == "passed" ? 0 : 1;
        completed = true;
    }

    [[nodiscard]] int exitCode() {
        std::scoped_lock lock{mutex};
        return status;
    }

    /// Throws unless `caller` is the page under test (the native capabilities answer to it only).
    void requireCaller(const Caller& caller) {
        std::scoped_lock lock{mutex};
        authorize(caller);
    }

private:
    void authorize(const Caller& caller) const {
        if (!browser || *browser != caller)
            throw std::runtime_error("Jasmine callback belongs to another page");
    }

    CaptureStore& captures(const Caller& caller) {
        {
            std::scoped_lock lock{mutex};
            authorize(caller);
        }
        if (!store)
            throw std::runtime_error("This test has no capture directory");
        return *store;  // Not under the mutex: a capture waits for the window and a recording runs for seconds.
    }

    std::vector<std::string>      files;
    std::filesystem::path         report;
    std::shared_ptr<CaptureStore> store;
    std::optional<Caller>         browser;
    std::mutex                    mutex;
    bool                          completed = false;
    int                           status    = 1;
};
}  // namespace detail

#ifdef WEBFRONT_JASMINE_TESTING
inline namespace jasmine_enabled {
/**
 * @brief Instruments the newest connected page only when WEBFRONT_JASMINE_CONFIG is set by CTest.
 * Compiled only where webfront_add_jasmine_test defined WEBFRONT_JASMINE_TESTING on the target; any other build gets an
 * inert class, so no environment variable can turn a distribution binary into a script loader. Define the macro
 * consistently for every translation unit of a program.
 * Construct before show(), retain until after shutdown(). Runtime and spec files are classic JavaScript; precompile
 * TS/modules in the consuming project. CTest's external process deadline also stops synchronous renderer hangs.
 * Exactly one window is supported: if the page reloads, the new connection is the one under test.
 */
template <typename App>
class JasmineTest {
    using AppUI   = typename App::UI;
    using Session = detail::JasmineSession<AppUI>;

public:
    explicit JasmineTest(App& app) {
        const auto path = detail::jasmineConfigurationPath();
        if (!path)
            return;
        const auto               config = detail::jasmineConfiguration(detail::readJasmineFile(*path));
        std::vector<std::string> contents;
        for (const auto& file : config.files)
            contents.push_back(detail::readJasmineFile(file));
        std::shared_ptr<CaptureStore> store;
        if (!config.captures.empty())
            store = std::make_shared<CaptureStore>(config.captures);
        session          = std::make_shared<Session>(std::move(contents), config.report, std::move(store));
        const auto owned = session;
        // Native capabilities exist only when the owner's CMake file granted some (PROGRAMS): the runner then sets this file.
        if (const auto hostPath = detail::jasmineEnvironment("WEBFRONT_JASMINE_HOST"))
            host = std::make_shared<HostSession>(detail::hostConfiguration(detail::readJasmineFile(*hostPath)));
        registerHost(app);
        app.cppFunction("webfront.testing.count", [owned](AppUI ui) {
            return owned->count(ui);
        });
        app.cppFunction("webfront.testing.read", [owned](AppUI ui, double index, double offset) {
            return owned->read(ui, index, offset);
        });
        app.cppFunction("webfront.testing.result", [owned](AppUI ui, std::string result) {
            owned->result(ui, result);
        });
        app.cppFunction("webfront.testing.screenshotStart", [owned](AppUI ui, std::string name) {
            owned->screenshotStart(ui, name);
        });
        app.cppFunction("webfront.testing.screenshotPoll", [owned](AppUI ui) {
            return owned->screenshotPoll(ui);
        });
        app.cppFunction("webfront.testing.recordStart", [owned](AppUI ui, std::string name, double rate) {
            owned->recordStart(ui, name, rate);
        });
        app.cppFunction("webfront.testing.recordStop", [owned](AppUI ui) {
            owned->recordStop(ui);
        });
        app.cppFunction("webfront.testing.recordPoll", [owned](AppUI ui) {
            return owned->recordPoll(ui);
        });
        subscription = app.subscribeUIStarted([owned](AppUI ui) {
            if (owned->attach(ui))
                ui.addScript(detail::jasmineScript());
        });
    }

    [[nodiscard]] int exitCode() const {
        return session ? session->exitCode() : 0;
    }

private:
    /// The page's bridge to HostSession; each call is checked against the page under test, and refused with no grant.
    void registerHost(App& app) {
        const auto owned = session;
        const auto grant = host;
        const auto guard = [owned, grant](const AppUI& ui) -> HostSession& {
            owned->requireCaller(ui);
            if (!grant)
                throw std::runtime_error("No native capability was granted to this test: add PROGRAMS to webfront_add_jasmine_test");
            return *grant;
        };
        app.cppFunction("webfront.testing.hostPrograms", [guard](AppUI ui) {
            return guard(ui).programs();
        });
        app.cppFunction("webfront.testing.hostMakeTemp", [guard](AppUI ui) {
            return guard(ui).makeTemp();
        });
        app.cppFunction("webfront.testing.hostRemoveTemp", [guard](AppUI ui, std::string path) {
            guard(ui).removeTemp(path);
        });
        app.cppFunction("webfront.testing.hostWrite", [guard](AppUI ui, std::string path, double offset, std::vector<std::uint8_t> bytes) {
            guard(ui).writeFile(path, offset, bytes);
        });
        app.cppFunction("webfront.testing.hostRead", [guard](AppUI ui, std::string path, double offset) {
            return guard(ui).readFile(path, offset);
        });
        app.cppFunction("webfront.testing.hostRunStart", [guard](AppUI ui, std::string request) {
            return guard(ui).runStart(request);
        });
        app.cppFunction("webfront.testing.hostRunPoll", [guard](AppUI ui, double id) {
            return guard(ui).runPoll(id).value_or(std::string{});
        });
        app.cppFunction("webfront.testing.hostRunRead", [guard](AppUI ui, double id, double stream, double offset) {
            return guard(ui).runRead(id, stream, offset);
        });
        app.cppFunction("webfront.testing.hostRunCancel", [guard](AppUI ui, double id) {
            guard(ui).runCancel(id);
        });
        app.cppFunction("webfront.testing.hostRunRelease", [guard](AppUI ui, double id) {
            guard(ui).runRelease(id);
        });
    }

    std::shared_ptr<Session>     session;
    std::shared_ptr<HostSession> host;
    UISubscription               subscription;
};
}  // namespace jasmine_enabled
#else
inline namespace jasmine_inert {
/// Inert in a build that did not define WEBFRONT_JASMINE_TESTING: it registers nothing and reports success.
template <typename App>
class JasmineTest {
public:
    explicit JasmineTest(App&) {}
    [[nodiscard]] int exitCode() const {
        return 0;
    }
};
}  // namespace jasmine_inert
#endif
}  // namespace webfront::testing
