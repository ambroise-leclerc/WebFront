/// @brief Intent journal of the developer store: publishing several files as one all-or-nothing change.
#pragma once

#include "WindowsCompat.hpp"
#include "DeveloperPaths.hpp"

#include <cstdio>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace webfront::developer {

/// Project-relative directory holding the journals of group publications.
inline constexpr std::string_view journalDirectory = ".webfront/developer/journal";
inline constexpr std::size_t      maxGroupFiles    = 4;

/// Called with the name of each step of a group publication and of its recovery. Tests throw
/// InjectedFault from it to stop the process at that step without any cleanup, as a crash would.
using StepHook = std::function<void(std::string_view)>;

/// A simulated crash: the store leaves the disk exactly as it is.
struct InjectedFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Flushes a file's content to stable storage.
inline void syncFile(const std::filesystem::path& file) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot open " + file.string() + " for synchronization");
    const bool flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    if (!flushed)
        throw std::runtime_error("Cannot synchronize " + file.string());
#else
    const int descriptor = ::open(file.c_str(), O_RDONLY);
    if (descriptor < 0)
        throw std::runtime_error("Cannot open " + file.string() + " for synchronization");
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    const bool flushed = ::fcntl(descriptor, F_FULLFSYNC) == 0 || ::fsync(descriptor) == 0;
#else
    const bool flushed = ::fsync(descriptor) == 0;
#endif
    ::close(descriptor);
    if (!flushed)
        throw std::runtime_error("Cannot synchronize " + file.string());
#endif
}

/// Makes the renames and creations in a directory durable. Windows offers no equivalent.
inline void syncDirectory(const std::filesystem::path& directory) {
#if !defined(_WIN32)
    syncFile(directory);
#else
    (void)directory;
#endif
}

/// What a group publication changes: the project file it replaces and the journal-owned file replacing it.
struct JournalEntry {
    std::string target;
    std::string source;
};

inline constexpr std::string_view journalHeader = "webfront-journal 1";
inline constexpr std::string_view journalEnd    = "end";

inline std::string serializeJournal(const std::vector<JournalEntry>& entries) {
    std::string text{journalHeader};
    text += '\n';
    for (const auto& [target, source] : entries)
        text += target + '\n' + source + '\n';
    text += journalEnd;
    text += '\n';
    return text;
}

/// Returns the entries of a complete journal, nothing for an incomplete or malformed one.
inline std::optional<std::vector<JournalEntry>> parseJournal(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream       stream{text};
    for (std::string line; std::getline(stream, line);)
        lines.push_back(std::move(line));
    if (lines.size() < 4 || lines.size() % 2 != 0 || lines.front() != journalHeader || lines.back() != journalEnd)
        return std::nullopt;
    std::vector<JournalEntry> entries;
    for (std::size_t i = 1; i + 1 < lines.size(); i += 2) {
        const auto& source = lines[i + 1];
        if (source.empty() || !std::ranges::all_of(source, [](unsigned char c) { return std::isdigit(c) != 0; }))
            return std::nullopt;
        entries.push_back({lines[i], source});
    }
    return entries;
}

/// Identifies the content of project files: a precondition for publications that were prepared
/// from an earlier reading of the project. Not a security measure.
inline std::string fingerprint(const std::filesystem::path& root, const std::vector<std::string>& paths) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto    feed = [&hash](const char* data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            hash = (hash ^ static_cast<unsigned char>(data[i])) * 1099511628211ULL;
    };
    for (const auto& path : paths) {
        const auto file = projectPath(root, path);
        std::ifstream stream(file, std::ios::binary);
        const char    present = std::filesystem::is_regular_file(file) && stream ? 1 : 0;
        feed(&present, 1);
        if (!present)
            continue;
        const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(file));
        feed(reinterpret_cast<const char*>(&size), sizeof size);
        std::vector<char> block(64 * 1024);
        while (stream.read(block.data(), static_cast<std::streamsize>(block.size())) || stream.gcount() > 0)
            feed(block.data(), static_cast<std::size_t>(stream.gcount()));
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

/// Completes a committed journal: every source still present replaces its target. Safe to repeat
/// after an interruption, including one of this very function.
inline void rollForward(const std::filesystem::path& root, const std::filesystem::path& directory, const std::string& number,
                        const std::vector<JournalEntry>& entries, const StepHook& hook) {
    const auto owned = directory / (number + ".d");
    int        done  = 0;
    std::set<std::filesystem::path> directories{owned};
    for (const auto& [target, source] : entries) {
        const auto staged      = owned / source;
        const auto destination = projectPath(root, target);
        if (std::filesystem::is_regular_file(staged)) {
            std::filesystem::create_directories(destination.parent_path());
            std::filesystem::rename(staged, destination);
        }
        directories.insert(destination.parent_path());
        if (hook)
            hook("replacement-" + std::to_string(++done));
    }
    // The journal is the only record of what remains to be done: it must outlive every replacement, so
    // make the renames durable (the directories holding the targets and the emptied source) first.
    for (const auto& changed : directories)
        syncDirectory(changed);
    if (hook)
        hook("replacements-synced");
    std::filesystem::remove(directory / (number + ".journal"));
    syncDirectory(directory);
    std::error_code ignored;
    std::filesystem::remove_all(owned, ignored);
}

/// Completes the journals that reached their commit point and abandons the others. A journal is
/// committed once it is renamed to "<n>.journal"; everything else is the remains of a publication
/// interrupted before that point, where no project file was touched.
inline void recoverJournals(const std::filesystem::path& root, const StepHook& hook = {}) {
    std::filesystem::path directory;
    try {
        directory = projectPath(root, journalDirectory);
    } catch (const std::exception&) {
        return;
    }
    std::error_code error;
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        found.push_back(entry.path());
    if (error)
        return;
    std::ranges::sort(found);
    std::set<std::string> committed;
    for (const auto& path : found) {
        if (path.extension() != ".journal")
            continue;
        std::ifstream      stream(path, std::ios::binary);
        std::ostringstream content;
        content << stream.rdbuf();
        const auto entries = parseJournal(content.str());
        if (!entries)
            continue;
        try {
            for (const auto& entry : *entries)
                projectPath(root, entry.target);
        } catch (const std::exception&) {
            continue;
        }
        committed.insert(path.stem().string());
        rollForward(root, directory, path.stem().string(), *entries, hook);
    }
    for (const auto& path : found) {
        const auto name = path.stem().string();
        if (!committed.contains(name))
            std::filesystem::remove_all(path, error);
    }
}

}  // namespace webfront::developer
