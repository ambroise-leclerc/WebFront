/// @brief Per-user discovery of the automation sessions their owners chose to share.
///
/// A shared session publishes one small record in a private per-user directory so that a human or an agent can ask
/// `webfront-agent --list` instead of being told where to attach. A record names the project, the grants, the process and
/// the private endpoint directory. It never holds the attachment capability, which stays in that endpoint directory:
/// listing a session does not allow operating it. Records are removed on closure; one whose process is gone is dropped.
#pragma once
#include "Platform.hpp"

#include "Channel.hpp"
#include "Json.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
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
    /// The JSON the CLI prints; the same members as the record, plus the observed state.
    std::string json() const {
        return objectJson({
            {"version",                                     "1"},
            {"session",                   webfront::json::quote(session)},
            {"pid",                              std::to_string(pid)},
            {"project",                   webfront::json::quote(project)},
            {"allow",                                         allow},
            {"endpoint",                 webfront::json::quote(endpoint)},
            {"state",                       webfront::json::quote("running")},
        });
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

/// Creates the directory when missing. On POSIX it must be ours and closed to other users; a shared or foreign
/// directory is refused rather than trusted.
inline void preparePrivateDirectory(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
#ifndef _WIN32
    struct stat info{};
    if (::lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())
        throw std::runtime_error("The automation discovery directory must be a directory owned by the current user");
    if ((info.st_mode & 077) != 0 && ::chmod(directory.c_str(), 0700) != 0)
        throw std::runtime_error("Cannot make the automation discovery directory private");
#endif
}

/// The record of one shared session, present as long as this object lives.
class Registration {
public:
    Registration() = default;
    Registration(const std::filesystem::path& directory, const Entry& entry) {
        if (!validIdentifier(entry.session))
            throw std::invalid_argument("Invalid automation session identifier");
        preparePrivateDirectory(directory);
        file = directory / (entry.session + ".json");
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        channel::writeEndpoint(file, entry.json());
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
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::filesystem::remove(file.string() + ".new", ignored);
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
        entry.session  = record.string("session");
        entry.project  = record.string("project");
        entry.endpoint = record.string("endpoint");
        entry.allow    = record.raw("allow", "[]");
        const auto pid = record.raw("pid", "0");
        entry.pid      = std::stol(pid);
        if (record.raw("version") != "1" || !validIdentifier(entry.session) || path.stem().string() != entry.session || entry.project.empty()
            || entry.endpoint.empty() || !entry.allow.starts_with('[') || entry.pid <= 0)
            return std::nullopt;
        Object{"{\"allow\":" + entry.allow + "}"};  // the grants must be well-formed JSON of the usual bounds
        return entry;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// The live shared sessions, ordered by identifier. A record of a vanished process is removed when `prune` is true;
/// malformed files and foreign names are ignored and never deleted. No record carries the attachment capability.
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
            }
            continue;
        }
        result.push_back(*entry);
    }
    std::ranges::sort(result, {}, &Entry::session);
    return result;
}

inline std::string listJson(const std::vector<Entry>& entries) {
    std::string result = "{\"version\":1,\"sessions\":[";
    for (const auto& entry : entries)
        result += (result.back() == '[' ? "" : ",") + entry.json();
    return result + "]}";
}

/// A ready-to-use `.mcp.json` for a client: one stdio server attached to an endpoint directory. It holds neither
/// grants (the owner's process fixes them) nor the capability (read from the endpoint at attach time).
inline std::string mcpConfiguration(const std::filesystem::path& agent, const std::filesystem::path& endpoint, std::string_view name = "webfront") {
    if (name.empty() || !std::ranges::all_of(name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }))
        throw std::invalid_argument("The MCP server name is letters, digits, '-' and '_'");
    const auto server = objectJson({
        {"command",                                                   webfront::json::quote(agent.generic_string())},
        {   "args", stringArray({"--attach", endpoint.generic_string(), "--mcp"})},
    });
    return "{\n  \"mcpServers\": {\n    " + webfront::json::quote(name) + ": " + server + "\n  }\n}\n";
}
}  // namespace webfront::automation::discovery
