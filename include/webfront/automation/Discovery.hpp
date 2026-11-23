// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-08
//
// WebFront — automation
// Role: Per-user discovery of the automation sessions their owners chose to share, as records in a private directory.
// Thread: worker-safe | Alloc: heap-init
// LLM: records never hold the token; stale records are dropped by the next listing

// Per-user discovery of the automation sessions their owners chose to share.
//
// An application that enables automation publishes one small record in a private per-user directory, so that a human or
// an agent finds the running instances instead of being told where they are. A record names the project, the grants, the
// process, the private directory, the MCP URL and the path of the client configuration. It never holds the token, which
// only that private configuration file contains: reading a record does not allow operating the session. Records are
// removed on closure; one whose process is gone is dropped by the next listing, with the private directory named after it.

#pragma once
#include "Channel.hpp"
#include "Json.hpp"
#include "Platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#ifndef _WIN32
    #include <csignal>
#endif
#include <string>
#include <vector>

namespace webfront::automation::discovery {
inline constexpr std::size_t recordLimit = 8192;

/// The directory of this user's records: %LOCALAPPDATA%\WebFront\sessions on Windows, ~/Library/Application
/// Support/WebFront/sessions on macOS, $XDG_STATE_HOME/webfront/sessions or ~/.local/state/webfront/sessions elsewhere.
inline std::filesystem::path defaultDirectory() {
    const auto variable = [](const char* name) -> std::string {
#ifdef _WIN32
        char*  value = nullptr;
        size_t size  = 0;
        if (_dupenv_s(&value, &size, name) != 0 || !value)
            return {};
        std::string result{value};
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value ? value : "";
#endif
    };
#ifdef _WIN32
    if (const auto base = variable("LOCALAPPDATA"); !base.empty())
        return std::filesystem::path{base} / "WebFront" / "sessions";
    if (const auto base = variable("USERPROFILE"); !base.empty())
        return std::filesystem::path{base} / "AppData" / "Local" / "WebFront" / "sessions";
#elif defined(__APPLE__)
    if (const auto home = variable("HOME"); !home.empty())
        return std::filesystem::path{home} / "Library" / "Application Support" / "WebFront" / "sessions";
#else
    if (const auto state = variable("XDG_STATE_HOME"); !state.empty() && std::filesystem::path{state}.is_absolute())
        return std::filesystem::path{state} / "webfront" / "sessions";
    if (const auto home = variable("HOME"); !home.empty())
        return std::filesystem::path{home} / ".local" / "state" / "webfront" / "sessions";
#endif
    throw std::runtime_error("Cannot locate the per-user directory of automation sessions");
}

/// A session identifier names the record file: only plain alphanumeric names are ever opened.
inline bool validIdentifier(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

/// One shared session as its owner published it. `allow` is the JSON array of its grants.
struct Entry {
    std::string session, project, endpoint, allow;
    long        pid = 0;
    /// The JSON array of the application callbacks that `call` may invoke (empty unless the owner allow-listed some).
    std::string callbacks = "[]";
    /// The MCP URL of the session (ADR-025) and the path of its private client configuration. Neither holds the token, which
    /// only the configuration file contains.
    std::string mcp{}, configuration{};
    /// The record as it is written.
    std::string json() const {
        std::map<std::string, std::string> members{
            {  "version",                              "1"},
            {  "session",   webfront::json::quote(session)},
            {      "pid",              std::to_string(pid)},
            {  "project",   webfront::json::quote(project)},
            {    "allow",                            allow},
            {"callbacks",                        callbacks},
            { "endpoint",  webfront::json::quote(endpoint)},
            {    "state", webfront::json::quote("running")},
        };
        if (!mcp.empty()) {
            members.emplace("mcp", webfront::json::quote(mcp));
            members.emplace("configuration", webfront::json::quote(configuration));
        }
        return objectJson(members);
    }
};

/// Whether a process still exists. A recycled identifier can keep a dead session listed until its record is replaced.
inline bool processAlive(long pid) noexcept {
    if (pid <= 0)
        return false;
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD      code  = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

inline long currentProcess() noexcept {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(::getpid());
#endif
}

/// Creates the directory when missing, private to the user on POSIX. An existing directory must already be ours and
/// closed to other users: a shared or foreign one is refused, never trusted nor changed, so its permissions and the
/// files it holds are left as they are. Whether the directory is new is read before creating it, not from the return value
/// of create_directories, which some libraries report as false for a path ending in a separator; the directory is then
/// opened without following a link, and only that descriptor is inspected and changed.
inline void preparePrivateDirectory(const std::filesystem::path& directory) {
    std::error_code probe;
    const bool      existed = std::filesystem::exists(std::filesystem::symlink_status(directory, probe));
    std::filesystem::create_directories(directory);
#ifndef _WIN32
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
        throw std::runtime_error("The automation discovery directory must be a real directory");
    struct Closer {
        int fd;
        ~Closer() {
            ::close(fd);
        }
    } closer{descriptor};
    struct stat info{};
    if (::fstat(descriptor, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())
        throw std::runtime_error("The automation discovery directory must be a directory owned by the current user");
    if ((info.st_mode & 077) != 0) {
        if (existed)
            throw std::runtime_error("The automation discovery directory exists and is not private to the current user");
        if (::fchmod(descriptor, 0700) != 0)
            throw std::runtime_error("Cannot make the automation discovery directory private");
    }
#else
    (void)existed;
#endif
}

using channel::removeWithRetry;
using channel::transientRemovalFailure;

/// The record of one shared session, present as long as this object lives.
class Registration {
public:
    Registration() = default;
    Registration(const std::filesystem::path& directory, const Entry& entry) {
        if (!validIdentifier(entry.session))
            throw std::invalid_argument("Invalid automation session identifier");
        // A record readRecord would skip could be neither listed nor pruned: refuse it before publishing anything.
        const auto content = entry.json();
        if (content.size() > recordLimit)
            throw std::invalid_argument("The automation discovery record exceeds " + std::to_string(recordLimit) + " bytes; use shorter paths");
        preparePrivateDirectory(directory);
        file = directory / (entry.session + ".json");
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        channel::writeEndpoint(file, content);
    }
    Registration(const Registration&)            = delete;
    Registration& operator=(const Registration&) = delete;
    ~Registration() {
        remove();
    }
    /// Withdraws the record, as normal closure does. Safe to repeat and to call from another thread.
    void remove() noexcept {
        const std::scoped_lock lock{mutex};
        if (file.empty())
            return;
        removeWithRetry(file);
        removeWithRetry(file.string() + ".new");
        file.clear();
    }

private:
    std::mutex            mutex;
    std::filesystem::path file;
};

inline std::optional<Entry> readRecord(const std::filesystem::path& path) {
    std::error_code error;
    const auto      status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status))
        return std::nullopt;
#ifndef _WIN32
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || info.st_uid != ::geteuid())
        return std::nullopt;
#endif
    std::ifstream input(path, std::ios::binary);
    std::string   text(recordLimit + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    if (text.empty() || text.size() > recordLimit)
        return std::nullopt;
    try {
        const Object record{text, recordLimit};
        Entry        entry;
        entry.session       = record.string("session");
        entry.project       = record.string("project");
        entry.endpoint      = record.string("endpoint");
        entry.allow         = record.raw("allow", "[]");
        entry.callbacks     = record.raw("callbacks", "[]");
        entry.mcp           = record.string("mcp");
        entry.configuration = record.string("configuration");
        const auto pid      = record.raw("pid", "0");
        entry.pid           = std::stol(pid);
        if (record.raw("version") != "1" || !validIdentifier(entry.session) || path.stem().string() != entry.session || entry.project.empty()
            || entry.endpoint.empty() || !entry.allow.starts_with('[') || !entry.callbacks.starts_with('[') || entry.pid <= 0)
            return std::nullopt;
        Object{"{\"allow\":" + entry.allow + "}"};  // the grants must be well-formed JSON of the usual bounds
        Object{"{\"callbacks\":" + entry.callbacks + "}"};
        return entry;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// The live shared sessions, ordered by identifier. A record of a vanished process is removed when `prune` is true;
/// malformed files and foreign names are ignored and never deleted. No record carries the automation token.
inline std::vector<Entry> list(const std::filesystem::path& directory, bool prune = true) {
    std::vector<Entry> result;
    std::error_code    error;
    if (!std::filesystem::is_directory(directory, error))
        return result;
#ifndef _WIN32
    struct stat info{};
    if (::lstat(directory.c_str(), &info) != 0 || info.st_uid != ::geteuid() || (info.st_mode & 077) != 0)
        return result;  // a directory other users could write is not trusted to name sessions
#endif
    for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
        const auto& path = item.path();
        if (path.extension() != ".json" || !validIdentifier(path.stem().string()))
            continue;
        const auto entry = readRecord(path);
        if (!entry)
            continue;
        if (!processAlive(entry->pid)) {
            if (prune) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                // The private directory created beside the record (ADR-025) goes with it: only its known files, then the
                // directory if it is empty. A directory elsewhere, named by the owner, is never touched. The record holds
                // the resolved path (channel::outsideProject), so the discovery directory is resolved the same way, and
                // the session's directory must be a real child of it, not a link put in its place.
                std::error_code unresolved;
                const auto      own  = std::filesystem::weakly_canonical(std::filesystem::absolute(directory), unresolved) / entry->session;
                const auto      kind = std::filesystem::symlink_status(own, unresolved);
                if (!unresolved && std::filesystem::is_directory(kind)
                    && std::filesystem::path(entry->endpoint).lexically_normal().generic_string() == own.lexically_normal().generic_string()) {
                    for (const auto* name : {"endpoint.json", "endpoint.json.new", "mcp.json", "mcp.json.new"})
                        std::filesystem::remove(own / name, ignored);
                    std::filesystem::remove(own, ignored);
                }
            }
            continue;
        }
        result.push_back(*entry);
    }
    std::ranges::sort(result, {}, &Entry::session);
    return result;
}

}  // namespace webfront::automation::discovery
