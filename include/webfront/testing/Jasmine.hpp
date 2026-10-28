/// @brief Opt-in Jasmine instrumentation of a consumer's real page, enabled by a CTest configuration.
#pragma once

#include "../utils/Json.hpp"
#include "../WebFront.hpp"

#include <cmath>
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
};

inline JasmineConfiguration jasmineConfiguration(std::string_view text) {
    JasmineConfiguration config;
    json::Reader         reader{text};
    bool                 files = false, report = false;
    reader.object([&](const std::string& key) {
        if (key == "files" && !std::exchange(files, true))
            reader.array([&] {
                if (config.files.size() >= 101)
                    throw std::invalid_argument("At most 100 Jasmine spec files are supported");
                config.files.push_back(reader.string());
            });
        else if (key == "report" && !std::exchange(report, true))
            config.report = reader.string();
        else
            throw std::invalid_argument("Unknown or duplicate Jasmine configuration field");
    });
    reader.end();
    if (config.files.size() < 2 || config.report.empty())
        throw std::invalid_argument("Jasmine needs its runtime, specs and report path");
    return config;
}

inline std::optional<std::string> jasmineConfigurationPath() {
#ifdef _MSC_VER
    char*       raw  = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "WEBFRONT_JASMINE_CONFIG") != 0)
        throw std::runtime_error("Cannot read WEBFRONT_JASMINE_CONFIG");
    std::unique_ptr<char, decltype(&std::free)> owned(raw, std::free);
    if (raw && *raw)
        return std::string(raw);
#else
    if (const auto* raw = std::getenv("WEBFRONT_JASMINE_CONFIG"); raw && *raw)
        return std::string(raw);
#endif
    return std::nullopt;
}

/// Bytes per bridge read, shared by the page script (injected as CHUNK) and JasmineSession::read.
inline constexpr std::size_t jasmineChunkSize = 49152;

inline constexpr std::string_view jasmineScriptBody = R"JS(
void (async () => {
    const call = (name, ...args) => webFront.cppFunction('webfront.testing.' + name)(...args);
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

/// The state shared by the bridge callbacks of one test run. Independent of the application type so it can be tested alone.
class JasmineSession {
public:
    static constexpr std::size_t chunkSize = jasmineChunkSize;

    JasmineSession(std::vector<std::string> contents, std::filesystem::path reportPath)
        : files(std::move(contents)), report(std::move(reportPath)) {}

    /// The newest page is the one under test (a reload replaces the previous connection). Returns false once finished.
    bool attach(WebLinkId id) {
        std::scoped_lock lock{mutex};
        if (completed)
            return false;
        if (browser && *browser != id)
            std::cerr << "Jasmine: a new page replaced the instrumented one\n";
        browser = id;
        return true;
    }

    [[nodiscard]] double count(WebLinkId caller) {
        std::scoped_lock lock{mutex};
        authorize(caller);
        return static_cast<double>(files.size());
    }

    [[nodiscard]] std::vector<std::uint8_t> read(WebLinkId caller, double index, double offset) {
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

    void result(WebLinkId caller, const std::string& text) {
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

private:
    void authorize(WebLinkId caller) const {
        if (!browser || *browser != caller)
            throw std::runtime_error("Jasmine callback belongs to another page");
    }

    std::vector<std::string> files;
    std::filesystem::path    report;
    std::optional<WebLinkId> browser;
    std::mutex               mutex;
    bool                     completed = false;
    int                      status    = 1;
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
public:
    explicit JasmineTest(App& app) {
        const auto path = detail::jasmineConfigurationPath();
        if (!path)
            return;
        const auto config = detail::jasmineConfiguration(detail::readJasmineFile(*path));
        std::vector<std::string> contents;
        for (const auto& file : config.files)
            contents.push_back(detail::readJasmineFile(file));
        session          = std::make_shared<detail::JasmineSession>(std::move(contents), config.report);
        using AppUI      = typename App::UI;
        const auto owned = session;
        app.cppFunction("webfront.testing.count", [owned](AppUI ui) { return owned->count(ui.id()); });
        app.cppFunction("webfront.testing.read", [owned](AppUI ui, double index, double offset) { return owned->read(ui.id(), index, offset); });
        app.cppFunction("webfront.testing.result", [owned](AppUI ui, std::string result) { owned->result(ui.id(), result); });
        subscription = app.subscribeUIStarted([owned](AppUI ui) {
            if (owned->attach(ui.id()))
                ui.addScript(detail::jasmineScript());
        });
    }

    [[nodiscard]] int exitCode() const {
        return session ? session->exitCode() : 0;
    }

private:
    std::shared_ptr<detail::JasmineSession> session;
    UISubscription                          subscription;
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
