/// @brief Writable storage confined to a root the application declares: the contract the services of ADR-020 read and
/// write through, whatever stores the bytes (a desktop file system, a simulated tree, FatFs or LittleFS on a device).
#pragma once

#include "FileSystem.hpp"
#include "SafePath.hpp"
#include <webfront/BridgeError.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace webfront::fs {

/// Why a storage operation failed. Each value has a stable bridge code (storageErrorCode()).
enum class StorageErrc {
    notFound,             ///< no entry has this name
    alreadyExists,        ///< the name is taken and the operation does not replace
    notDirectory,         ///< a component that must be a directory is not one
    isDirectory,          ///< a file operation names a directory
    notEmpty,             ///< removing a directory that still has entries
    accessDenied,         ///< the system or the storage refuses the access
    invalidPath,          ///< the path fails the syntax of SafePath, or names the root where an entry is needed
    linkRefused,          ///< a component is a link the root does not follow, or one that leaves the root
    unsafeDirectory,      ///< a directory of the root, or an ancestor, can be moved by another user (POSIX)
    rootMoved,            ///< the declared location no longer designates the root opened at declaration
    confinementViolated,  ///< a directory left the root during the operation: detected, the operation was abandoned
    busy,                 ///< another process holds the entry in a way that prevents the operation (Windows sharing)
    ioError,              ///< any other failure of the underlying storage
};

/// The bridge code of a storage error, `storage.<name>` (for example `storage.rootMoved`).
[[nodiscard]] constexpr std::string_view storageErrorCode(StorageErrc errc) noexcept {
    switch (errc) {
        case StorageErrc::notFound:
            return "storage.notFound";
        case StorageErrc::alreadyExists:
            return "storage.alreadyExists";
        case StorageErrc::notDirectory:
            return "storage.notDirectory";
        case StorageErrc::isDirectory:
            return "storage.isDirectory";
        case StorageErrc::notEmpty:
            return "storage.notEmpty";
        case StorageErrc::accessDenied:
            return "storage.accessDenied";
        case StorageErrc::invalidPath:
            return "storage.invalidPath";
        case StorageErrc::linkRefused:
            return "storage.linkRefused";
        case StorageErrc::unsafeDirectory:
            return "storage.unsafeDirectory";
        case StorageErrc::rootMoved:
            return "storage.rootMoved";
        case StorageErrc::confinementViolated:
            return "storage.confinementViolated";
        case StorageErrc::busy:
            return "storage.busy";
        case StorageErrc::ioError:
            break;
    }
    return "storage.ioError";
}

/**
 * @brief The failure of a storage operation. It is a BridgeError: a service callback that lets it propagate rejects the
 * JavaScript Promise with an `Error` whose `code` is storageErrorCode(errc()).
 *
 * The message names the path relative to the root, never the root's location on the host (ADR-020: a host path never
 * reaches JavaScript unless the application sends it).
 */
class StorageError : public BridgeError {
public:
    StorageError(StorageErrc errc, const std::string& message) : BridgeError(std::string(storageErrorCode(errc)), message), errcValue(errc) { }

    [[nodiscard]] StorageErrc errc() const noexcept {
        return errcValue;
    }

private:
    StorageErrc errcValue;
};

enum class EntryType {
    file,
    directory,
    link,  ///< a symbolic link or another reparse point, listed without being followed
    other  ///< a device, a socket, a pipe: never opened
};

/// What a listing or status() reports of an entry.
struct EntryInfo {
    std::string                           name;    ///< the entry's name in its directory; empty for the root
    EntryType                             type{EntryType::other};
    std::uint64_t                         size{};  ///< in bytes, for a file; 0 otherwise
    std::chrono::system_clock::time_point modified{};
    /// Indicative rights of the running process, derived from the entry's attributes (POSIX mode bits for the owner,
    /// the group and the others; the read-only attribute on Windows; ACLs are not examined). An operation can still
    /// be refused with accessDenied.
    bool readable{};
    bool writable{};

    bool operator==(const EntryInfo&) const = default;
};

/// One page of a directory listing.
struct DirectoryPage {
    std::vector<EntryInfo> entries;  ///< sorted by name, byte by byte
    /// Present when entries remain: pass it as `after` to list() to read the next page.
    std::optional<std::string> next;
};

/// What committing a write does when the name exists.
enum class WriteMode {
    createNew,  ///< refuse with alreadyExists
    replace     ///< replace the file atomically; a directory is never replaced
};

/// The number of entries list() returns at most, whatever its caller asks.
inline constexpr std::size_t maxStoragePageSize = 1000;

/// A write in progress: the bytes go to a temporary entry, invisible under the final name until commit() renames it
/// atomically. Destroying an uncommitted writer removes the temporary entry, so an interrupted write leaves no partial
/// file behind.
template <typename W>
concept StorageWriter = requires(W writer, std::span<const char> bytes) {
    writer.write(bytes);
    writer.commit();
};

/**
 * @brief A writable storage confined to one root.
 *
 * Paths are '/'-separated and relative to the root, checked by safePathComponents() (the empty path is the root);
 * names starting with temporaryPrefix are reserved to the temporary files of writes.
 * Every operation is resolved inside the root and fails with a StorageError; a storage never follows a component
 * outside its root.
 *
 * - `list(dir, after, limit)`: the entries of a directory whose names sort after `after` (empty: from the first),
 *   at most `limit` (bounded by maxStoragePageSize). Pages are stable: an entry present during the whole
 *   listing appears exactly once, whatever is added or removed meanwhile.
 * - `status(path)`: the entry itself, a link not followed.
 * - `openRead(path)`: the content of a file.
 * - `beginWrite(path, mode)`: a StorageWriter.
 * - `createDirectory(path)`, `remove(path)` (a file, a link or an empty directory), `rename(from, to)` (never
 *   replaces).
 */
template <typename S>
concept Storage = requires(S& storage, std::string_view path, std::size_t limit, std::span<const char> bytes) {
    { storage.list(path, path, limit) } -> std::same_as<DirectoryPage>;
    { storage.status(path) } -> std::same_as<EntryInfo>;
    { storage.openRead(path) } -> std::same_as<File>;
    { storage.beginWrite(path, WriteMode::createNew) } -> StorageWriter;
    storage.createDirectory(path);
    storage.remove(path);
    storage.rename(path, path);
};

/// The prefix a storage reserves for the temporary files of its writes in progress: no path may have a component that
/// starts with it, in any letter case (invalidPath), and listings omit such entries. The case does not matter because
/// case-insensitive volumes (macOS by default, Windows) name the same file with ".WF-TMP-": the prefix is ASCII, and no
/// other character folds to its letters.
inline constexpr std::string_view temporaryPrefix = ".wf-tmp-";

/// True when a name starts with temporaryPrefix, whatever the case of its letters.
[[nodiscard]] inline bool isTemporaryName(std::string_view name) noexcept {
    if (name.size() < temporaryPrefix.size())
        return false;
    for (std::size_t index = 0; index < temporaryPrefix.size(); ++index) {
        const char c = name[index];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != temporaryPrefix[index])
            return false;
    }
    return true;
}

namespace detail {

/// The components of a path given to a storage operation, or StorageError(invalidPath).
inline std::vector<std::string> storageComponents(std::string_view path) {
    std::vector<std::string> components;
    try {
        components = safePathComponents(path);
    } catch (const std::invalid_argument&) {
        throw StorageError(StorageErrc::invalidPath, "Invalid storage path '" + std::string(path) + "'");
    }
    for (const auto& component : components)
        if (isTemporaryName(component))
            throw StorageError(StorageErrc::invalidPath, "Names starting with '" + std::string(temporaryPrefix) + "' are reserved");
    return components;
}

/// The components of a path that must name an entry, not the root.
inline std::vector<std::string> entryComponents(std::string_view path) {
    auto components = storageComponents(path);
    if (components.empty())
        throw StorageError(StorageErrc::invalidPath, "The operation needs an entry, not the root");
    return components;
}

/// Sorts a whole directory and keeps the page after `after`.
inline DirectoryPage pageOf(std::vector<EntryInfo> entries, std::string_view after, std::size_t limit) {
    if (!after.empty() && !isSafeFileName(after))
        throw StorageError(StorageErrc::invalidPath, "Invalid listing cursor");
    limit = std::clamp<std::size_t>(limit, 1, maxStoragePageSize);
    std::ranges::sort(entries, {}, &EntryInfo::name);
    auto          first = after.empty() ? entries.begin() : std::ranges::upper_bound(entries, after, {}, &EntryInfo::name);
    DirectoryPage page;
    const auto    count = std::min<std::size_t>(limit, static_cast<std::size_t>(entries.end() - first));
    page.entries.assign(std::make_move_iterator(first), std::make_move_iterator(first + static_cast<std::ptrdiff_t>(count)));
    if (first + static_cast<std::ptrdiff_t>(count) != entries.end())
        page.next = page.entries.back().name;
    return page;
}

/// Joins components for a message.
inline std::string joinComponents(const std::vector<std::string>& components) {
    std::string joined;
    for (const auto& component : components)
        joined += (joined.empty() ? "" : "/") + component;
    return joined;
}

}  // namespace detail

}  // namespace webfront::fs
