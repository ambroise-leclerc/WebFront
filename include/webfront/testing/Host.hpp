/// @brief Native capabilities granted by a test's owner to its specs: authorized programs and private temporary spaces.
#pragma once

#include "../utils/Json.hpp"
#include "Process.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace webfront::testing {

namespace detail {

/// What the owner of the test (its CMake file, through an environment variable the runner sets) grants the specs.
struct HostConfiguration {
    std::vector<std::filesystem::path> programs;   ///< Absolute paths of the only programs the specs may run.
    std::filesystem::path              temporary;  ///< Directory holding every temporary space of this run.
};

inline std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

inline HostConfiguration hostConfiguration(std::string_view text) {
    HostConfiguration config;
    json::Reader      reader{text};
    bool              programs = false, temporary = false;
    reader.object([&](const std::string& key) {
        if (key == "programs" && !std::exchange(programs, true))
            reader.array([&] {
                if (config.programs.size() >= 64)
                    throw std::invalid_argument("At most 64 programs can be authorized");
                auto path = pathFromUtf8(reader.string());
                if (!path.is_absolute())
                    throw std::invalid_argument("An authorized program must be an absolute path");
                config.programs.push_back(std::move(path));
            });
        else if (key == "temporary" && !std::exchange(temporary, true))
            config.temporary = pathFromUtf8(reader.string());
        else
            throw std::invalid_argument("Unknown or duplicate host configuration field");
    });
    reader.end();
    if (!config.temporary.is_absolute())
        throw std::invalid_argument("The host configuration needs an absolute temporary directory");
    return config;
}

/// One request from a spec, after validation.
struct HostRunRequest {
    std::filesystem::path program;
    ProcessSpec           spec;
};

inline double jsonNumber(json::Reader& reader) {
    const auto token        = reader.rawValue();
    double     value        = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value))
        throw std::invalid_argument("A number was expected");
    return value;
}

inline bool jsonBoolean(json::Reader& reader) {
    const auto token = reader.rawValue();
    if (token == "true")
        return true;
    if (token == "false")
        return false;
    throw std::invalid_argument("A boolean was expected");
}

}  // namespace detail

/**
 * @brief The native side of `webFrontHost`: runs only the programs its owner authorized and confines files to temporary
 * spaces it created. This is an explicit grant, not a sandbox: an authorized program runs with the user's authority.
 * Thread-safe. Destruction cancels and joins every run, then removes every temporary space.
 */
class HostSession {
public:
    static constexpr std::size_t               chunkSize       = 49152;
    static constexpr std::size_t               maxActiveRuns   = 8;
    static constexpr std::size_t               maxRetainedRuns = 64;
    static constexpr std::size_t               maxArguments    = 256;
    static constexpr std::size_t               maxOutputLimit  = 16 * 1024 * 1024;
    static constexpr std::size_t               maxFileBytes    = 64 * 1024 * 1024;
    static constexpr std::chrono::milliseconds maxTimeout{3600 * 1000};

    explicit HostSession(detail::HostConfiguration configuration) : config(std::move(configuration)) {
        std::error_code error;
        std::filesystem::create_directories(config.temporary, error);
        if (error)
            throw std::runtime_error("Cannot create the temporary directory: " + error.message());
        config.temporary = std::filesystem::weakly_canonical(config.temporary);
        // Private to this user (POSIX): every space below inherits the closed doorway, whatever the umask.
        std::filesystem::permissions(config.temporary, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
    }
    HostSession(const HostSession&)            = delete;
    HostSession& operator=(const HostSession&) = delete;

    ~HostSession() {
        std::map<double, std::shared_ptr<Run>> remaining;
        {
            std::scoped_lock lock{mutex};
            remaining.swap(runs);
        }
        for (auto& [id, run] : remaining)
            run->cancel = true;
        for (auto& [id, run] : remaining)
            if (run->worker.joinable())
                run->worker.join();
        std::set<std::filesystem::path> leftover;
        {
            std::scoped_lock lock{mutex};
            leftover.swap(spaces);
        }
        for (const auto& space : leftover)
            removeTree(space);
    }

    /// The authorized programs, as a JSON array of strings.
    [[nodiscard]] std::string programs() const {
        std::string text = "[";
        for (const auto& program : config.programs) {
            if (text.size() > 1)
                text += ',';
            text += json::quote(detail::utf8(program));
        }
        return text + "]";
    }

    /// Creates a new empty directory, private to this run, and returns its absolute path.
    [[nodiscard]] std::string makeTemp() {
        static std::atomic<std::uint64_t> counter{0};
        std::random_device                entropy;
        for (int attempt = 0; attempt < 16; ++attempt) {
            char name[64];
            std::snprintf(name, sizeof name, "t%08x%08x-%llu", entropy(), entropy(), static_cast<unsigned long long>(counter++));
            const auto      path = config.temporary / name;
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) {
                std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
                std::scoped_lock lock{mutex};
                if (spaces.size() >= 256) {
                    std::filesystem::remove(path, error);
                    throw std::runtime_error("Too many temporary spaces");
                }
                spaces.insert(path);
                return detail::utf8(path);
            }
            if (error)
                throw std::runtime_error("Cannot create a temporary space: " + error.message());
        }
        throw std::runtime_error("Cannot find a free temporary space name");
    }

    /// Removes a space made by makeTemp() and everything in it.
    void removeTemp(const std::string& path) {
        const auto space = detail::pathFromUtf8(path);
        {
            std::scoped_lock lock{mutex};
            if (!spaces.erase(space))
                throw std::invalid_argument("Not a temporary space of this run");
        }
        removeTree(space);
    }

    /// Writes bytes at `offset` of a file inside a temporary space. Offset 0 truncates; later chunks must continue the file.
    void writeFile(const std::string& path, double offset, const std::vector<std::uint8_t>& bytes) {
        const auto file = contained(path);
        if (!std::isfinite(offset) || offset < 0 || std::floor(offset) != offset
            || offset + static_cast<double>(bytes.size()) > static_cast<double>(maxFileBytes))
            throw std::invalid_argument("Invalid file offset");
        const auto      start = static_cast<std::uintmax_t>(offset);
        std::error_code error;
        if (start != 0 && (!std::filesystem::is_regular_file(file, error) || std::filesystem::file_size(file, error) != start))
            throw std::invalid_argument("A chunk must continue the file");
        std::ofstream out(file, start == 0 ? std::ios::binary | std::ios::trunc : std::ios::binary | std::ios::app);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write the file");
    }

    /// Reads at most one chunk at `offset`; a short (or empty) result is the end of the file.
    [[nodiscard]] std::vector<std::uint8_t> readFile(const std::string& path, double offset) {
        const auto    file = contained(path);
        std::ifstream in(file, std::ios::binary);
        if (!in)
            throw std::runtime_error("Cannot read the file");
        if (!std::isfinite(offset) || offset < 0 || std::floor(offset) != offset)
            throw std::invalid_argument("Invalid file offset");
        in.seekg(static_cast<std::streamoff>(offset));
        std::vector<std::uint8_t> bytes(chunkSize);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(in.gcount()));
        return bytes;
    }

    /// Validates the request, refuses an unauthorized program, and starts the run on its own thread.
    /// Throws for anything refused; otherwise returns the run's id.
    [[nodiscard]] double runStart(const std::string& request) {
        auto             parsed = parse(request);
        std::scoped_lock lock{mutex};
        std::size_t      active = 0;
        for (const auto& [id, run] : runs)
            if (!run->finished)
                ++active;
        if (active >= maxActiveRuns || runs.size() >= maxRetainedRuns)
            throw std::runtime_error("Too many runs: collect and release the finished ones");
        auto run  = std::make_shared<Run>();
        run->id   = ++lastId;
        run->spec = std::move(parsed.spec);
        // A raw pointer would dangle once the session is destroyed: the thread owns a reference.
        run->worker = std::thread([run] {
            ProcessResult outcome;
            try {
                outcome = runProcess(run->spec, run->cancel);
            } catch (const std::exception& error) {
                outcome.error = error.what();
            }
            std::scoped_lock done{run->mutex};
            run->result   = std::move(outcome);
            run->finished = true;
        });
        runs.emplace(run->id, run);
        return run->id;
    }

    /// The run's summary as JSON once it ended, or nothing while it is still running.
    [[nodiscard]] std::optional<std::string> runPoll(double id) {
        const auto       run = find(id);
        std::scoped_lock lock{run->mutex};
        if (!run->finished)
            return std::nullopt;
        const auto& r    = run->result;
        std::string text = "{\"status\":" + json::quote(processStatusName(r.status));
        text            += ",\"exitCode\":" + std::to_string(r.exitCode) + ",\"signal\":" + std::to_string(r.signal);
        text            += ",\"error\":" + json::quote(r.error.substr(0, 1024));
        text            += ",\"stdoutBytes\":" + std::to_string(r.standardOutput.size()) + ",\"stderrBytes\":" + std::to_string(r.standardError.size());
        text            += std::string(",\"stdoutTruncated\":") + (r.outputTruncated ? "true" : "false");
        text            += std::string(",\"stderrTruncated\":") + (r.errorTruncated ? "true" : "false");
        text            += ",\"durationMs\":" + std::to_string(r.duration.count()) + "}";
        return text;
    }

    /// One chunk of a finished run's captured output: stream 1 is standard output, 2 standard error.
    [[nodiscard]] std::vector<std::uint8_t> runRead(double id, double stream, double offset) {
        const auto       run = find(id);
        std::scoped_lock lock{run->mutex};
        if (!run->finished)
            throw std::logic_error("The run has not ended");
        if (stream != 1 && stream != 2)
            throw std::invalid_argument("Invalid stream");
        const auto& text = stream == 1 ? run->result.standardOutput : run->result.standardError;
        if (!std::isfinite(offset) || offset < 0 || std::floor(offset) != offset || offset > static_cast<double>(text.size()))
            throw std::invalid_argument("Invalid output offset");
        const auto part = std::string_view(text).substr(static_cast<std::size_t>(offset), chunkSize);
        return {part.begin(), part.end()};
    }

    /// Asks a running program to stop; its kill is prompt and its result says `cancelled`.
    void runCancel(double id) {
        find(id)->cancel = true;
    }

    /// Frees a run's result (cancelling it first if it still runs).
    void runRelease(double id) {
        std::shared_ptr<Run> run;
        {
            std::scoped_lock lock{mutex};
            const auto       found = runs.find(id);
            if (found == runs.end())
                throw std::invalid_argument("Unknown run");
            run = found->second;
            runs.erase(found);
        }
        run->cancel = true;
        if (run->worker.joinable())
            run->worker.join();
    }

private:
    struct Run {
        double            id = 0;
        ProcessSpec       spec;
        std::atomic<bool> cancel{false};
        std::mutex        mutex;
        std::atomic<bool> finished{false};
        ProcessResult     result;
        std::thread       worker;
    };

    detail::HostRunRequest parse(const std::string& request) {
        detail::HostRunRequest parsed;
        parsed.spec.timeout = std::chrono::milliseconds{30000};
        std::string  program, directory;
        double       timeout = 30000, output = 1024 * 1024;
        bool         seenProgram = false;
        json::Reader reader{request};
        reader.object([&](const std::string& key) {
            if (key == "program") {
                program     = reader.string();
                seenProgram = true;
            } else if (key == "args")
                reader.array([&] {
                    if (parsed.spec.arguments.size() >= maxArguments)
                        throw std::invalid_argument("Too many arguments");
                    parsed.spec.arguments.push_back(reader.string());
                });
            else if (key == "cwd")
                directory = reader.string();
            else if (key == "env")
                reader.object([&](const std::string& name) {
                    parsed.spec.environment[name] = reader.string();
                });
            else if (key == "inheritEnv")
                parsed.spec.inheritEnvironment = detail::jsonBoolean(reader);
            else if (key == "timeoutMs")
                timeout = detail::jsonNumber(reader);
            else if (key == "maxOutputBytes")
                output = detail::jsonNumber(reader);
            else
                throw std::invalid_argument("Unknown run option: " + key);
        });
        reader.end();
        if (!seenProgram)
            throw std::invalid_argument("A program is required");
        if (!(timeout >= 1 && timeout <= static_cast<double>(maxTimeout.count())) || std::floor(timeout) != timeout)
            throw std::invalid_argument("timeoutMs must be a whole number from 1 to 3600000");
        if (!(output >= 1 && output <= static_cast<double>(maxOutputLimit)) || std::floor(output) != output)
            throw std::invalid_argument("maxOutputBytes must be a whole number from 1 to 16777216");
        parsed.spec.timeout        = std::chrono::milliseconds{static_cast<std::int64_t>(timeout)};
        parsed.spec.maxOutputBytes = static_cast<std::size_t>(output);

        // Only the owner's list decides; the program that starts is the owner's path, not the spec's spelling.
        const auto requested = detail::pathFromUtf8(program);
        parsed.spec.program.clear();
        for (const auto& allowed : config.programs) {
            std::error_code error;
            if (requested.is_absolute() && std::filesystem::equivalent(allowed, requested, error) && !error) {
                parsed.spec.program = allowed;
                break;
            }
        }
        if (parsed.spec.program.empty())
            throw std::runtime_error("Program not authorized: " + program);
        if (!directory.empty())
            parsed.spec.workingDirectory = contained(directory, true);
        return parsed;
    }

    /// The path, if it lies inside a temporary space of this run (symbolic links resolved).
    std::filesystem::path contained(const std::string& path, bool allowSpace = false) {
        const auto requested = detail::pathFromUtf8(path);
        if (!requested.is_absolute())
            throw std::invalid_argument("A temporary path must be absolute");
        std::error_code error;
        const auto      resolved = std::filesystem::weakly_canonical(requested, error);
        if (error)
            throw std::invalid_argument("Invalid path: " + error.message());
        std::scoped_lock lock{mutex};
        for (const auto& space : spaces) {
            const auto [spaceEnd, resolvedEnd] = std::mismatch(space.begin(), space.end(), resolved.begin(), resolved.end());
            if (spaceEnd == space.end() && (allowSpace || resolvedEnd != resolved.end()))
                return resolved;  // Inside; the space itself is a directory, not a file.
        }
        throw std::invalid_argument("The path is outside the temporary spaces of this run");
    }

    std::shared_ptr<Run> find(double id) {
        std::scoped_lock lock{mutex};
        const auto       found = runs.find(id);
        if (found == runs.end())
            throw std::invalid_argument("Unknown run");
        return found->second;
    }

    /// Windows may hold a file a moment after its process ended.
    static void removeTree(const std::filesystem::path& path) noexcept {
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (!error && !std::filesystem::exists(path, error))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }

    detail::HostConfiguration              config;
    std::mutex                             mutex;
    std::set<std::filesystem::path>        spaces;
    std::map<double, std::shared_ptr<Run>> runs;
    double                                 lastId = 0;
};

}  // namespace webfront::testing
