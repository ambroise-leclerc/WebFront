#pragma once

#include <webfront/utils/Json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace webfront::testdriver {
struct SuiteCase {
    std::string              name;
    std::vector<std::string> command;
    int                      expected = 0;
    std::chrono::seconds     timeout{100};
};

// A deliberately small schema: expected status is a decimal string, commands are argv arrays, never shell text.
inline std::vector<SuiteCase> parseSuite(std::string_view source, std::string_view filter = {}) {
    std::vector<SuiteCase> cases;
    std::set<std::string>  names;
    json::Reader           reader{source};
    reader.array([&] {
        SuiteCase item;
        reader.object([&](const std::string& key) {
            if (key == "name")
                item.name = reader.string();
            else if (key == "command")
                reader.array([&] {
                    item.command.push_back(reader.string());
                });
            else if (key == "expect") {
                const auto value        = reader.string();
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), item.expected);
                if (error != std::errc{} || end != value.data() + value.size() || item.expected < 0 || item.expected > 255)
                    throw std::invalid_argument("Suite expect must be a decimal exit status from 0 to 255");
            } else if (key == "timeout") {
                const auto value        = reader.string();
                long       seconds      = 0;
                const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (error != std::errc{} || end != value.data() + value.size() || seconds < 1 || seconds > 86400)
                    throw std::invalid_argument("Suite timeout must be whole seconds from 1 to 86400");
                item.timeout = std::chrono::seconds(seconds);
            } else
                throw std::invalid_argument("Unknown suite member: " + key);
        });
        if (item.name.empty() || item.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos
            || !names.insert(item.name).second || item.command.empty())
            throw std::invalid_argument("Suite cases need unique file-safe names and nonempty commands");
        if (filter.empty() || item.name.find(filter) != std::string::npos)
            cases.push_back(std::move(item));
    });
    reader.end();
    if (cases.empty())
        throw std::invalid_argument("No suite case matches the filter");
    return cases;
}

// The native watchdog keeps both the whole-session budget and the original per-case limit, including when
// a spec blocks JavaScript so neither the parent frame nor a JS timer can report an expired command.
class DriverDeadline {
public:
    explicit DriverDeadline(std::chrono::steady_clock::duration total) : overall(std::chrono::steady_clock::now() + total), current(overall) {}
    void beginCase(std::chrono::steady_clock::duration timeout) {
        std::scoped_lock lock{mutex};
        current = std::min(overall, std::chrono::steady_clock::now() + timeout);
        ++revision;
        changed.notify_all();
    }
    void endCase() {
        std::scoped_lock lock{mutex};
        current = overall;
        ++revision;
        changed.notify_all();
    }
    bool expired(std::stop_token stop) {
        std::unique_lock lock{mutex};
        while (!stop.stop_requested()) {
            const auto previous = revision;
            const auto limit    = current;
            if (!changed.wait_until(lock, stop, limit, [&] {
                    return previous != revision;
                }))
                return !stop.stop_requested();
        }
        return false;
    }

private:
    const std::chrono::steady_clock::time_point overall;
    std::chrono::steady_clock::time_point       current;
    std::size_t                                 revision = 0;
    std::mutex                                  mutex;
    std::condition_variable_any                 changed;
};

inline std::string readSuite(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::invalid_argument("Cannot read suite: " + path.string());
    const auto size = std::filesystem::file_size(path);
    if (size > 1024 * 1024)
        throw std::invalid_argument("Suite exceeds 1 MiB: " + path.string());
    std::string source(static_cast<std::size_t>(size), '\0');
    if (!source.empty() && !file.read(source.data(), static_cast<std::streamsize>(source.size())))
        throw std::invalid_argument("Truncated suite: " + path.string());
    return source;
}
}  // namespace webfront::testdriver
