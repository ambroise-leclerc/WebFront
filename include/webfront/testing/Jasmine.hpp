/// @brief Opt-in Jasmine instrumentation of a consumer's real page, enabled by a CTest configuration.
#pragma once

#include "../utils/Json.hpp"
#include "../WebFront.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

inline constexpr std::string_view jasmineScript = R"JS(
void (async () => {
    const call = (name, ...args) => webFront.cppFunction('webfront.testing.' + name)(...args);
    const load = async index => {
        const parts = [];
        for (let offset = 0; ; offset += 49152) {
            const part = await call('read', index, offset);
            parts.push(part);
            if (part.length < 49152) break;
        }
        const script = document.createElement('script');
        script.textContent = await new Blob(parts).text();
        document.head.append(script);
        script.remove();
    };
    let reporting = false;
    const report = async result => {
        if (reporting) return;
        reporting = true;
        try { await call('result', JSON.stringify(result)); }
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
            specDone(result) { if (result.status !== 'excluded') ++total; if (result.status === 'failed') ++failed; record(result); },
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
        await report({format:1, status:'error', error:String(error?.stack ?? error).slice(0,8192)});
    }
})();
)JS";
}  // namespace detail

/**
 * @brief Instruments the first connected page only when WEBFRONT_JASMINE_CONFIG is set by CTest.
 * Construct before show(), retain until after shutdown(). Include/construct only in a test-enabled build.
 * Runtime and spec files are classic JavaScript; precompile TS/modules in the consuming project.
 * CTest's external process deadline also stops synchronous renderer hangs.
 */
template <typename App>
class JasmineTest {
    struct State {
        std::vector<std::string> files;
        std::filesystem::path    report;
        std::optional<WebLinkId> browser;
        std::mutex               mutex;
        bool                     completed = false;
        int                      status    = 1;
        void                     authorize(const typename App::UI& ui) const {
            if (!browser || *browser != ui.id())
                throw std::runtime_error("Jasmine callback belongs to another page");
        }
    };

public:
    explicit JasmineTest(App& app) {
        const auto path = detail::jasmineConfigurationPath();
        if (!path)
            return;
        const auto config = detail::jasmineConfiguration(detail::readJasmineFile(*path));
        state             = std::make_shared<State>();
        state->report     = config.report;
        for (const auto& file : config.files)
            state->files.push_back(detail::readJasmineFile(file));
        using AppUI      = typename App::UI;
        const auto owned = state;
        app.cppFunction("webfront.testing.count", [owned](AppUI ui) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui);
            return static_cast<double>(owned->files.size());
        });
        app.cppFunction("webfront.testing.read", [owned](AppUI ui, double index, double offset) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui);
            if (!std::isfinite(index) || index < 0 || index >= static_cast<double>(owned->files.size()) || std::floor(index) != index)
                throw std::invalid_argument("Invalid Jasmine file index");
            const auto& file = owned->files[static_cast<std::size_t>(index)];
            if (!std::isfinite(offset) || offset < 0 || offset > static_cast<double>(file.size()) || std::floor(offset) != offset)
                throw std::invalid_argument("Invalid Jasmine file offset");
            const auto part = std::string_view(file).substr(static_cast<std::size_t>(offset), 49152);
            return std::vector<std::uint8_t>(part.begin(), part.end());
        });
        app.cppFunction("webfront.testing.result", [owned](AppUI ui, std::string result) {
            std::scoped_lock lock{owned->mutex};
            owned->authorize(ui);
            if (owned->completed)
                throw std::logic_error("Jasmine already completed");
            std::string  status;
            bool         seen = false;
            json::Reader reader{result};
            reader.object([&](const std::string& key) {
                if (key == "status") {
                    if (std::exchange(seen, true))
                        throw std::invalid_argument("Duplicate Jasmine status");
                    status = reader.string();
                } else
                    reader.skipValue();
            });
            reader.end();
            if (status != "passed" && status != "failed" && status != "error")
                throw std::invalid_argument("Invalid Jasmine result");
            std::ofstream file(owned->report, std::ios::binary | std::ios::trunc);
            file << result << '\n';
            file.close();
            if (!file)
                throw std::runtime_error("Cannot write Jasmine report");
            owned->status    = status == "passed" ? 0 : 1;
            owned->completed = true;
        });
        subscription = app.subscribeUIStarted([owned](AppUI ui) {
            std::scoped_lock lock{owned->mutex};
            if (owned->browser)
                return;
            owned->browser = ui.id();
            ui.addScript(detail::jasmineScript);
        });
    }

    [[nodiscard]] int exitCode() const {
        if (!state)
            return 0;
        std::scoped_lock lock{state->mutex};
        return state->status;
    }

private:
    std::shared_ptr<State> state;
    UISubscription         subscription;
};
}  // namespace webfront::testing
