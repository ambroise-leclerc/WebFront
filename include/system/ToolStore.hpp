/// @brief Installed web tools (the wf tool manager, #234): validation of the names and paths a
/// browser sends, the files of each tool under <project>/.webfront/tools/, and the lock file
/// <project>/webfront-tools.lock.json that records them.
#pragma once

#include "../utils/Json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace webfront::fs {

/// One installed tool, as recorded in the lock file.
struct ToolRecord {
    std::string name{};
    std::string version{};
    std::string integrity{};
    std::string entry{};  ///< Page opened by 'wf open', relative to the package root
    std::string title{};

    bool operator==(const ToolRecord&) const = default;
};

/// Part of a file sent by the browser: bytes to write at an offset of a '/'-separated path.
struct FileChunk {
    std::string_view              path{};
    std::uint64_t                 offset = 0;
    std::span<const std::uint8_t> bytes{};
};

namespace tools {

namespace detail {

inline constexpr std::string_view nameCharacters    = "abcdefghijklmnopqrstuvwxyz0123456789-._~";
inline constexpr std::string_view versionCharacters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.+-";

inline std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline bool consistsOf(std::string_view text, std::string_view allowed) {
    return text.find_first_not_of(allowed) == std::string_view::npos;
}

// Neither "." nor ".." can be a name part, since no part starts with '.'.
inline bool isNamePart(std::string_view part) {
    if (part.empty())
        return false;
    return std::string_view("._").find(part.front()) == std::string_view::npos && consistsOf(part, nameCharacters);
}

inline bool isPackageName(std::string_view name) {
    if (name.size() > 214)
        return false;
    if (!name.starts_with('@'))
        return isNamePart(name);
    const auto slash = name.find('/');
    if (slash == std::string_view::npos)
        return false;
    return isNamePart(name.substr(1, slash - 1)) && isNamePart(name.substr(slash + 1));
}

inline bool isVersion(std::string_view version) {
    if (version.empty() || version.size() > 256)
        return false;
    return std::isdigit(static_cast<unsigned char>(version.front())) != 0 && consistsOf(version, versionCharacters);
}

inline bool hasSafeCharacters(std::string_view path) {
    using namespace std::string_view_literals;
    return path.size() <= 4096 && path.find_first_of("\\:\0"sv) == std::string_view::npos;
}

// Appends a component unless it is empty, "." or ".." (or "...", which no package needs).
inline bool appendComponent(std::filesystem::path& path, std::string_view component) {
    if (component.find_first_not_of('.') == std::string_view::npos)
        return false;
    path /= fromUtf8(component);
    return true;
}

}  // namespace detail

/// Accepts an npm package name: lowercase URL-safe characters, an optional "@scope/" prefix, and no
/// part starting with '.' or '_'. Throws std::invalid_argument.
inline void validatePackageName(std::string_view name) {
    if (!detail::isPackageName(name))
        throw std::invalid_argument("Invalid package name '" + std::string(name) + "'");
}

/// Accepts a semantic version: a digit followed by letters, digits, '.', '+' or '-'. Throws
/// std::invalid_argument.
inline void validateVersion(std::string_view version) {
    if (!detail::isVersion(version))
        throw std::invalid_argument("Invalid package version '" + std::string(version) + "'");
}

/// Converts a '/'-separated path received from the browser into a relative path that stays inside
/// its directory. Rejects empty and absolute paths, empty, "." and ".." components, backslashes,
/// colons (drive letters and alternate streams) and NUL characters with std::invalid_argument.
inline std::filesystem::path safeRelativePath(std::string_view path) {
    std::filesystem::path result;
    bool                  safe = detail::hasSafeCharacters(path);
    for (const auto part : std::views::split(path, '/'))
        safe = safe && detail::appendComponent(result, std::string_view(part.begin(), part.end()));
    if (!safe || result.empty())
        throw std::invalid_argument("Unsafe path '" + std::string(path) + "'");
    return result;
}

namespace detail {

// The string members of a lock file record, by JSON name.
inline constexpr std::array<std::pair<std::string_view, std::string ToolRecord::*>, 4> recordFields{
    {
     {"version", &ToolRecord::version},
     {"integrity", &ToolRecord::integrity},
     {"entry", &ToolRecord::entry},
     {"title", &ToolRecord::title},
     }
};

inline void validateRecord(const ToolRecord& record) {
    validatePackageName(record.name);
    validateVersion(record.version);
    safeRelativePath(record.entry);
}

inline ToolRecord readRecord(json::Reader& reader, const std::string& name) {
    ToolRecord record{.name = name};
    reader.object([&](const std::string& key) {
        const auto field = std::ranges::find(recordFields, key, &std::pair<std::string_view, std::string ToolRecord::*>::first);
        if (field == recordFields.end())
            return reader.skipValue();
        record.*(field->second) = reader.string();
    });
    validateRecord(record);
    return record;
}

}  // namespace detail

using LockedTools = std::map<std::string, ToolRecord, std::less<>>;

/// Parses a lock file. Unknown members are ignored; every recorded name, version and entry is
/// validated, so a hand-edited lock file cannot point outside the tools directory. Throws
/// std::runtime_error on malformed JSON and std::invalid_argument on an invalid record.
inline LockedTools parseLock(std::string_view text) {
    LockedTools  tools;
    json::Reader reader{text};
    reader.object([&](const std::string& key) {
        if (key != "tools")
            return reader.skipValue();
        reader.object([&](const std::string& name) {
            tools.insert_or_assign(name, detail::readRecord(reader, name));
        });
    });
    reader.end();
    return tools;
}

/// Writes a lock file's JSON, tools sorted by name.
inline std::string formatLock(const LockedTools& tools) {
    std::string text      = "{\n  \"lockfileVersion\": 1,\n  \"tools\": {";
    const char* separator = "\n";
    for (const auto& [name, record] : tools) {
        text                      += separator;
        text                      += "    " + json::quote(name) + ": {";
        const char* fieldSeparator = "\n";
        for (const auto& [field, member] : detail::recordFields) {
            text          += fieldSeparator;
            text          += "      " + json::quote(field) + ": " + json::quote(record.*member);
            fieldSeparator = ",\n";
        }
        text     += "\n    }";
        separator = ",\n";
    }
    return text += tools.empty() ? "}\n}\n" : "\n  }\n}\n";
}

}  // namespace tools

/// The tools installed in a project directory:
///   <project>/.webfront/tools/<name>@<version>/   the files of each installed tool
///   <project>/webfront-tools.lock.json            name, version, integrity, entry and title of each
/// An installation is staged under .webfront/tools/.staging/ and replaces the installed version
/// only when committed; a reinstalled version is kept under .webfront/tools/.backup/ until the lock
/// file is written. A failed or abandoned installation leaves the installed tool intact.
/// Thread-safe within a process. Errors throw std::invalid_argument (invalid input) or
/// std::runtime_error / std::filesystem::filesystem_error (I/O).
class ToolStore {
public:
    explicit ToolStore(std::filesystem::path projectDirectory) : project(std::move(projectDirectory)) {}

    [[nodiscard]] const std::filesystem::path& projectDir() const {
        return project;
    }
    [[nodiscard]] std::filesystem::path toolsDir() const {
        return project / ".webfront" / "tools";
    }
    [[nodiscard]] std::filesystem::path lockFile() const {
        return project / "webfront-tools.lock.json";
    }
    [[nodiscard]] std::filesystem::path installDir(std::string_view name, std::string_view version) const {
        return toolsDir() / tools::detail::fromUtf8(std::string(name) + "@" + std::string(version));
    }

    /// The installed tools; an absent lock file means none. Throws std::runtime_error if the lock
    /// file cannot be read or is malformed, so its tools are never dropped by mistake.
    [[nodiscard]] tools::LockedTools installed() const {
        return readLock(lockFile());
    }

    [[nodiscard]] std::optional<ToolRecord> find(std::string_view name) const {
        auto tools = installed();
        if (auto found = tools.find(name); found != tools.end())
            return std::move(found->second);
        return std::nullopt;
    }

    /// Starts (or restarts) the installation of a version, discarding the files already staged for it.
    void begin(std::string_view name, std::string_view version) {
        tools::validatePackageName(name);
        tools::validateVersion(version);
        std::scoped_lock lock{mutex};
        std::filesystem::remove_all(stagingDir(name, version));
        pending.insert(key(name, version));
    }

    /// Writes a chunk of a staged file. Offset 0 creates or truncates the file; any other offset must
    /// equal the number of bytes already written, so a missing or repeated chunk is detected.
    void write(std::string_view name, std::string_view version, const FileChunk& chunk) {
        const auto       relative = tools::safeRelativePath(chunk.path);
        std::scoped_lock lock{mutex};
        requirePending(name, version);
        const auto target = stagingDir(name, version) / relative;
        if (chunk.offset != 0)
            requireWritten(target, chunk);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary | (chunk.offset == 0 ? std::ios::trunc : std::ios::app));
        file.write(reinterpret_cast<const char*>(chunk.bytes.data()), static_cast<std::streamsize>(chunk.bytes.size()));
        if (!file.flush())
            throw std::runtime_error("Cannot write '" + std::string(chunk.path) + "'");
    }

    /// Installs the staged files as record.version, replacing any installed version of the tool, and
    /// records it in the lock file. The entry page must have been written.
    void commit(const ToolRecord& record) {
        tools::detail::validateRecord(record);
        std::scoped_lock lock{mutex};
        requirePending(record.name, record.version);
        const auto staged = stagingDir(record.name, record.version);
        if (!std::filesystem::is_regular_file(staged / tools::safeRelativePath(record.entry)))
            throw std::invalid_argument("Entry page '" + record.entry + "' of " + record.name + " was not written");

        auto                                 locked = readLock(lockFile());
        std::optional<std::filesystem::path> replaced;
        if (const auto previous = locked.find(record.name); previous != locked.end() && previous->second.version != record.version)
            replaced = installDir(record.name, previous->second.version);
        locked.insert_or_assign(record.name, record);
        installStaged(record, locked);
        pending.erase(key(record.name, record.version));
        if (replaced)
            std::filesystem::remove_all(*replaced);
    }

    /// Uninstalls a tool. Returns false if it is not installed.
    bool remove(std::string_view name) {
        tools::validatePackageName(name);
        std::scoped_lock lock{mutex};
        auto             tools = readLock(lockFile());
        const auto       found = tools.find(name);
        if (found == tools.end())
            return false;
        const auto directory = installDir(name, found->second.version);
        tools.erase(found);
        writeLock(tools);
        std::filesystem::remove_all(directory);
        return true;
    }

    static tools::LockedTools readLock(const std::filesystem::path& file) {
        if (!std::filesystem::exists(file))
            return {};
        std::ifstream stream(file, std::ios::binary);
        if (!stream.is_open())
            throw std::runtime_error("Cannot read " + file.filename().string());
        std::ostringstream content;
        content << stream.rdbuf();
        try {
            return tools::parseLock(content.view());
        } catch (const std::exception& error) {
            throw std::runtime_error("Malformed " + file.filename().string() + ": " + error.what());
        }
    }

private:
    std::filesystem::path project;
    std::mutex            mutex;
    std::set<std::string> pending;

    static std::string key(std::string_view name, std::string_view version) {
        return std::string(name) + "@" + std::string(version);
    }

    [[nodiscard]] std::filesystem::path stagingDir(std::string_view name, std::string_view version) const {
        return toolsDir() / ".staging" / tools::detail::fromUtf8(key(name, version));
    }

    static void requireWritten(const std::filesystem::path& target, const FileChunk& chunk) {
        std::error_code error;
        const auto      written = std::filesystem::file_size(target, error);
        if (error || written != chunk.offset)
            throw std::invalid_argument("Chunk of '" + std::string(chunk.path) + "' at offset " + std::to_string(chunk.offset)
                                        + " does not follow the bytes already written");
    }

    // Moves the staged files into place and writes the lock file. A directory already installed for
    // this version is kept aside until the lock file is written, and restored if anything fails.
    void installStaged(const ToolRecord& record, const tools::LockedTools& locked) const {
        const auto target = installDir(record.name, record.version);
        const auto backup = toolsDir() / ".backup" / tools::detail::fromUtf8(key(record.name, record.version));
        std::filesystem::remove_all(backup);
        const bool replacing = std::filesystem::exists(target);
        if (replacing) {
            std::filesystem::create_directories(backup.parent_path());
            std::filesystem::rename(target, backup);
        }
        try {
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::rename(stagingDir(record.name, record.version), target);
            writeLock(locked);
        } catch (...) {
            restore(target, backup, replacing);
            throw;
        }
        std::filesystem::remove_all(backup);
    }

    static void restore(const std::filesystem::path& target, const std::filesystem::path& backup, bool replacing) {
        std::error_code ignored;
        std::filesystem::remove_all(target, ignored);
        if (replacing)
            std::filesystem::rename(backup, target, ignored);
    }

    void requirePending(std::string_view name, std::string_view version) const {
        if (!pending.contains(key(name, version)))
            throw std::invalid_argument("No installation of " + key(name, version) + " in progress");
    }

    // Written next to the lock file, then renamed over it, so a reader never sees a partial file.
    void writeLock(const tools::LockedTools& tools) const {
        const auto file      = lockFile();
        auto       temporary = file;
        temporary           += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << tools::formatLock(tools);
            if (!stream.flush())
                throw std::runtime_error("Cannot write " + temporary.string());
        }
        std::filesystem::rename(temporary, file);
    }
};

}  // namespace webfront::fs
