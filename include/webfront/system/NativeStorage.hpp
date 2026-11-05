/// @brief Writable storage on the host's file system, confined to a declared root while resolving every path (ADR-002,
/// ADR-020): operations are relative to directories already opened, never a check of a path followed by an open by name.
#pragma once

#include "Storage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include "WindowsCompat.hpp"

    #include <array>
    #include <cstddef>
    #include <cstring>
    #include <iterator>

    #include <winternl.h>
#else
    #include <cerrno>

    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <stdio.h>
    #elif defined(__linux__)
        #include <sys/syscall.h>
    #endif
#endif

namespace webfront::fs {

/// What resolving a path does with a symbolic link (or, on Windows, any reparse point: links and junctions).
enum class LinkPolicy {
    refuse,       ///< any link on the path fails with linkRefused (the default)
    followInside  ///< a relative link is followed when its resolution stays inside the root (POSIX; refused on Windows)
};

/// How a root is declared.
struct RootOptions {
    /// Lifts the POSIX ownership checks of the root's directories and ancestors, for a shared folder or a removable
    /// medium mounted without permissions. The confinement is weakened: another user who can write in one of those
    /// directories can move it, and the storage's later operations with it, out of the root.
    bool       shared{false};
    LinkPolicy links{LinkPolicy::refuse};
};

/// The points of an operation where StorageHooks::at is called, so that a test injects a concurrent change exactly there.
enum class StoragePoint {
    componentOpened,   ///< a directory of the path was opened (argument: its name); the next one is not opened yet
    resolved,          ///< the path is resolved, the operation has no effect yet (argument: the final name)
    temporaryCreated,  ///< a write created its temporary file, before writing to it (argument: the final name)
    beforeCommit       ///< a commit is about to check the confinement, then rename (argument: the final name)
};

/// The POSIX owner and mode of a directory, as the ownership checks see them.
struct DirectoryFacts {
    std::uint32_t owner{};
    std::uint32_t mode{};
};

/**
 * @brief Injection points for the tests of a NativeStorage. Production code passes none.
 *
 * `at` runs on the thread of the operation, at the points StoragePoint lists. `effectiveUser` replaces the effective
 * user the POSIX checks admit, and `facts` may rewrite what fstat() reported of a directory (its absolute path is
 * given), so that a test simulates a directory owned by another user without being the superuser.
 */
struct StorageHooks {
    std::function<void(StoragePoint, std::string_view)>                at;
    std::optional<std::uint32_t>                                       effectiveUser;
    std::function<void(const std::filesystem::path&, DirectoryFacts&)> facts;
};

namespace detail {

inline constexpr std::string_view temporaryPrefix = ".wf-tmp-";

inline std::string temporaryName() {
    thread_local std::mt19937_64 random{std::random_device{}()};
    static constexpr char        digits[] = "0123456789abcdef";
    std::string                  name(temporaryPrefix);
    auto                         bits = random();
    for (int index = 0; index < 16; ++index, bits >>= 4)
        name += digits[bits & 0xF];
    return name;
}

inline std::string quoted(const std::vector<std::string>& components) {
    return "'" + joinComponents(components) + "'";
}

}  // namespace detail

/**
 * @brief A writable storage under a root directory of the host, which satisfies fs::Storage.
 *
 * The root is the canonical form of the location given at construction (links in it are resolved once, at the
 * declaration). Every operation then:
 * - checks that this location, resolved component by component without following links, still designates the
 *   directory opened at declaration; otherwise it fails with rootMoved, and every later operation too;
 * - resolves its path from the root's descriptor (POSIX) or handle (Windows), one component at a time, refusing links
 *   unless RootOptions::links follows them inside the root; no descriptor outlives the operation;
 * - POSIX: checks that each directory traversed, the root and its ancestors cannot be moved by another user: each must
 *   belong to the effective user or the superuser and be writable by neither the group nor the others, except an
 *   ancestor with the sticky bit whose next element belongs to one of them (the case of `/tmp`); otherwise
 *   unsafeDirectory, before any effect (RootOptions::shared lifts these checks);
 * - Windows: holds the root and every directory traversed without sharing their deletion, so that nobody can rename or
 *   remove them during the operation (and the root during the whole declaration);
 * - before committing a write and after every operation, checks that the directory it acted in is still under the root
 *   (up through `..` on POSIX) and that the root is still at its location; otherwise confinementViolated (the
 *   temporary file of a write is removed and the write not committed).
 *
 * Guarantee: on Windows, these directories cannot move during an operation. On POSIX, no other user than the effective
 * one and the superuser can move them; those two can, and the last check only detects a move it sees: a move between
 * that check and the rename, or during the write of the temporary file, can leave a file outside the root.
 *
 * Renames, commits, removals and directory creations of a storage are serialized. Entries whose name SafePath would
 * reject, and the temporary files of writes in progress, are not listed.
 */
class NativeStorage {
    struct State;

public:
    /// Declares a root.
    /// @throws StorageError notFound or notDirectory when the location is not an existing directory, unsafeDirectory
    /// when a POSIX check refuses it, linkRefused when it is a Windows reparse point.
    explicit NativeStorage(const std::filesystem::path& location, RootOptions options = {}, std::shared_ptr<const StorageHooks> hooks = {});

    /// The canonical location of the root, for the application. Never send it to JavaScript unless it should know it.
    [[nodiscard]] const std::filesystem::path& location() const;

    class Writer;

    DirectoryPage list(std::string_view path, std::string_view after = {}, std::size_t limit = maxStoragePageSize);
    EntryInfo     status(std::string_view path);
    File          openRead(std::string_view path);
    Writer        beginWrite(std::string_view path, WriteMode mode);
    void          createDirectory(std::string_view path);
    void          remove(std::string_view path);
    void          rename(std::string_view from, std::string_view to);

private:
    std::shared_ptr<State> state;
};

#ifndef _WIN32

namespace detail::posix {

    #if defined(__linux__) && defined(O_PATH)
inline constexpr int searchFlags = O_PATH | O_DIRECTORY;
    #elif defined(O_SEARCH)
inline constexpr int searchFlags = O_SEARCH;
    #else
inline constexpr int searchFlags = O_RDONLY | O_DIRECTORY;
    #endif

class Fd {
public:
    Fd() = default;
    explicit Fd(int descriptor) : descriptor(descriptor) { }
    Fd(Fd&& other) noexcept : descriptor(std::exchange(other.descriptor, -1)) { }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            descriptor = std::exchange(other.descriptor, -1);
        }
        return *this;
    }
    Fd(const Fd&)            = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return descriptor >= 0;
    }
    int release() noexcept {
        return std::exchange(descriptor, -1);
    }
    void reset() noexcept {
        if (descriptor >= 0)
            ::close(descriptor);
        descriptor = -1;
    }

private:
    int descriptor{-1};
};

struct Identity {
    dev_t device{};
    ino_t inode{};
    bool  operator==(const Identity&) const = default;
};

inline Identity identityOf(const struct stat& status) {
    return {status.st_dev, status.st_ino};
}

inline StorageErrc errcOf(int error) {
    switch (error) {
        case ENOENT:
            return StorageErrc::notFound;
        case EEXIST:
            return StorageErrc::alreadyExists;
        case ENOTDIR:
            return StorageErrc::notDirectory;
        case EISDIR:
            return StorageErrc::isDirectory;
        case ENOTEMPTY:
            return StorageErrc::notEmpty;
        case EACCES:
        case EPERM:
        case EROFS:
            return StorageErrc::accessDenied;
        case ELOOP:
            return StorageErrc::linkRefused;
        case EBUSY:
        case ETXTBSY:
            return StorageErrc::busy;
        case EINVAL:
        case ENAMETOOLONG:
            return StorageErrc::invalidPath;
        default:
            return StorageErrc::ioError;
    }
}

[[noreturn]] inline void fail(int error, const std::string& what) {
    throw StorageError(errcOf(error), what + ": " + std::generic_category().message(error));
}

inline Fd openAt(int directory, const char* name, int flags, mode_t mode = 0) {
    int descriptor;
    do
        descriptor = ::openat(directory, name, flags | O_CLOEXEC, mode);
    while (descriptor < 0 && errno == EINTR);
    return Fd{descriptor};
}

inline struct stat statOf(int descriptor, const std::string& what) {
    struct stat status{};
    if (::fstat(descriptor, &status) != 0)
        fail(errno, what);
    return status;
}

inline std::chrono::system_clock::time_point modifiedOf(const struct stat& status) {
    #if defined(__APPLE__)
    const auto& time = status.st_mtimespec;
    #else
    const auto& time = status.st_mtim;
    #endif
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds{time.tv_sec} + std::chrono::nanoseconds{time.tv_nsec})};
}

inline bool inGroup(gid_t group) {
    if (group == ::getegid())
        return true;
    const int count = ::getgroups(0, nullptr);
    if (count <= 0)
        return false;
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    const int          found = ::getgroups(count, groups.data());
    return found > 0 && std::ranges::find(groups.begin(), groups.begin() + found, group) != groups.begin() + found;
}

inline EntryInfo infoOf(std::string name, const struct stat& status) {
    EntryInfo info{.name = std::move(name), .modified = modifiedOf(status)};
    if (S_ISREG(status.st_mode)) {
        info.type = EntryType::file;
        info.size = static_cast<std::uint64_t>(status.st_size);
    } else if (S_ISDIR(status.st_mode))
        info.type = EntryType::directory;
    else if (S_ISLNK(status.st_mode))
        info.type = EntryType::link;
    const auto user = ::geteuid();
    if (user == 0) {
        info.readable = info.writable = true;
        return info;
    }
    const mode_t shift = status.st_uid == user ? 6 : inGroup(status.st_gid) ? 3 : 0;
    info.readable      = (status.st_mode >> shift) & 04;
    info.writable      = (status.st_mode >> shift) & 02;
    return info;
}

// The rename that refuses to replace: renameat2(RENAME_NOREPLACE) on Linux, renameatx_np(RENAME_EXCL) on macOS. On a
// Linux file system without it, a file is linked then unlinked (link() never replaces); a directory is checked then
// renamed, so a directory created meanwhile under the target name may be replaced if it is empty.
inline int renameNoReplace(int fromDirectory, const char* from, int toDirectory, const char* to) {
    #if defined(__APPLE__)
    return ::renameatx_np(fromDirectory, from, toDirectory, to, RENAME_EXCL);
    #else
        #if defined(SYS_renameat2)
    constexpr unsigned noReplace = 1;  // RENAME_NOREPLACE
    if (::syscall(SYS_renameat2, fromDirectory, from, toDirectory, to, noReplace) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return -1;
        #endif
    struct stat status{};
    if (::fstatat(fromDirectory, from, &status, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (!S_ISDIR(status.st_mode)) {
        if (::linkat(fromDirectory, from, toDirectory, to, 0) != 0)
            return -1;
        return ::unlinkat(fromDirectory, from, 0);
    }
    if (::fstatat(toDirectory, to, &status, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        return -1;
    }
    return ::renameat(fromDirectory, from, toDirectory, to);
    #endif
}

// The content of a file opened relative to a directory of the root.
class FdSource : public ByteSource {
public:
    FdSource(Fd file, std::uint64_t total) : file(std::move(file)), total(total) { }

    std::size_t read(std::span<char> buffer) override {
        std::size_t count = 0;
        while (count < buffer.size()) {
            const auto got = ::read(file.get(), buffer.data() + count, buffer.size() - count);
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0)
                fail(errno, "Cannot read the file");
            if (got == 0)
                break;
            count += static_cast<std::size_t>(got);
        }
        return count;
    }
    [[nodiscard]] std::optional<std::uint64_t> size() const override {
        return total;
    }
    [[nodiscard]] bool seek(std::uint64_t offset) override {
        return ::lseek(file.get(), static_cast<off_t>(std::min(offset, total)), SEEK_SET) >= 0;
    }

private:
    Fd            file;
    std::uint64_t total;
};

}  // namespace detail::posix

struct NativeStorage::State {
    using Fd       = detail::posix::Fd;
    using Identity = detail::posix::Identity;

    std::filesystem::path               location;
    std::vector<std::string>            locationComponents;
    RootOptions                         options;
    std::shared_ptr<const StorageHooks> hooks;
    Fd                                  root;
    Identity                            rootIdentity;
    std::uint32_t                       effectiveUser{};
    std::mutex                          structure;
    std::atomic<bool>                   moved{false};

    // The directories an operation holds, from the root down to the one it acts in.
    struct Walk {
        std::vector<Fd>          directories;
        std::vector<std::string> names;
        int                      links{};

        [[nodiscard]] int top() const {
            return directories.back().get();
        }
    };

    void notify(StoragePoint point, std::string_view name) const {
        if (hooks && hooks->at)
            hooks->at(point, name);
    }

    [[nodiscard]] bool admissible(std::uint32_t owner) const {
        return owner == effectiveUser || owner == 0;
    }

    DirectoryFacts factsOf(const struct stat& status, const std::filesystem::path& path) const {
        DirectoryFacts facts{static_cast<std::uint32_t>(status.st_uid), static_cast<std::uint32_t>(status.st_mode)};
        if (hooks && hooks->facts)
            hooks->facts(path, facts);
        return facts;
    }

    // A directory of the root, the root included: only an admissible owner may move its entries out.
    void checkTraversed(const DirectoryFacts& facts, const std::string& what) const {
        if (options.shared)
            return;
        if (!admissible(facts.owner) || (facts.mode & (S_IWGRP | S_IWOTH)) != 0)
            throw StorageError(StorageErrc::unsafeDirectory, what + " can be moved by another user");
    }

    // An ancestor of the root, and the element of the location that follows it.
    void checkAncestor(const DirectoryFacts& ancestor, const DirectoryFacts& next) const {
        if (options.shared)
            return;
        const bool othersWrite = (ancestor.mode & (S_IWGRP | S_IWOTH)) != 0;
        const bool sticky      = (ancestor.mode & S_ISVTX) != 0;
        if (!admissible(ancestor.owner) || (othersWrite && !(sticky && admissible(next.owner))))
            throw StorageError(StorageErrc::unsafeDirectory, "An ancestor of the root can be moved by another user");
    }

    // Opens the declared location from `/` without following links, checking every ancestor. `onMove` is the error a
    // location that no longer leads to a directory reports.
    Fd openLocation(StorageErrc onMove) const {
        Fd current = detail::posix::openAt(AT_FDCWD, "/", detail::posix::searchFlags);
        if (!current)
            detail::posix::fail(errno, "Cannot open the file system root");
        std::filesystem::path path  = "/";
        auto                  facts = factsOf(detail::posix::statOf(current.get(), "Cannot inspect an ancestor of the root"), path);
        for (const auto& component : locationComponents) {
            Fd next = detail::posix::openAt(current.get(), component.c_str(), detail::posix::searchFlags | O_NOFOLLOW);
            if (!next) {
                const int error = errno;
                if (error == EACCES || error == EPERM)
                    detail::posix::fail(error, "Cannot reach the root");
                throw StorageError(onMove, "The root is no longer at its declared location");
            }
            path                /= component;
            const auto nextFacts = factsOf(detail::posix::statOf(next.get(), "Cannot inspect an ancestor of the root"), path);
            checkAncestor(facts, nextFacts);
            facts   = nextFacts;
            current = std::move(next);
        }
        checkTraversed(facts, "The root");
        return current;
    }

    // Fails with rootMoved, now and for every later operation, when the declared location designates another directory.
    void verifyLocation() {
        if (moved)
            throw StorageError(StorageErrc::rootMoved, "The root is no longer at its declared location");
        try {
            const auto found = openLocation(StorageErrc::rootMoved);
            if (detail::posix::identityOf(detail::posix::statOf(found.get(), "Cannot inspect the root")) != rootIdentity)
                throw StorageError(StorageErrc::rootMoved, "The root is no longer at its declared location");
        } catch (const StorageError& error) {
            if (error.errc() == StorageErrc::rootMoved)
                moved = true;
            throw;
        }
    }

    // True when `directory` is the root or below it, walking up through "..".
    bool isUnderRoot(int directory) const {
        auto current = detail::posix::openAt(directory, ".", detail::posix::searchFlags);
        for (int depth = 0; current && depth < 4096; ++depth) {
            struct stat status{};
            if (::fstat(current.get(), &status) != 0)
                return false;
            const auto identity = detail::posix::identityOf(status);
            if (identity == rootIdentity)
                return true;
            auto        parent = detail::posix::openAt(current.get(), "..", detail::posix::searchFlags);
            struct stat parentStatus{};
            if (!parent || ::fstat(parent.get(), &parentStatus) != 0 || detail::posix::identityOf(parentStatus) == identity)
                return false;
            current = std::move(parent);
        }
        return false;
    }

    // The check that follows an operation and precedes a commit: confinementViolated when a directory left the root.
    void confirm(std::initializer_list<int> directories) {
        for (const int directory : directories)
            if (!isUnderRoot(directory))
                throw StorageError(StorageErrc::confinementViolated, "A directory left the root during the operation");
        try {
            verifyLocation();
        } catch (const StorageError& error) {
            if (error.errc() != StorageErrc::rootMoved)
                throw;
            throw StorageError(StorageErrc::confinementViolated, "The root moved during the operation");
        }
    }

    Walk start() {
        verifyLocation();
        Walk walk;
        auto top = detail::posix::openAt(root.get(), ".", detail::posix::searchFlags);
        if (!top)
            detail::posix::fail(errno, "Cannot open the root");
        walk.directories.push_back(std::move(top));
        return walk;
    }

    // Replaces the link `name` of the top directory by its target in `pending`, when the policy follows it.
    void spliceLink(Walk& walk, const std::string& name, std::deque<std::string>& pending) const {
        auto named = walk.names;
        named.push_back(name);
        const auto where = detail::quoted(named);
        if (options.links != LinkPolicy::followInside)
            throw StorageError(StorageErrc::linkRefused, where + " is a link");
        if (++walk.links > 32)
            throw StorageError(StorageErrc::linkRefused, "Too many links resolving " + where);
        std::string target(4096, '\0');
        const auto  length = ::readlinkat(walk.top(), name.c_str(), target.data(), target.size());
        if (length < 0)
            detail::posix::fail(errno, "Cannot read the link " + where);
        target.resize(static_cast<std::size_t>(length));
        if (target.empty() || target.front() == '/')
            throw StorageError(StorageErrc::linkRefused, "The link " + where + " is absolute");
        std::vector<std::string> parts;
        for (const auto part : std::views::split(std::string_view(target), '/'))
            if (!part.empty())
                parts.emplace_back(part.begin(), part.end());
        pending.insert(pending.begin(), parts.begin(), parts.end());
    }

    // Opens the directories of `pending` from the top of the walk until `keep` components remain.
    void descend(Walk& walk, std::deque<std::string>& pending, std::size_t keep) {
        while (pending.size() > keep) {
            auto name = std::move(pending.front());
            pending.pop_front();
            if (name == ".")
                continue;
            if (name == "..") {
                if (walk.names.empty())
                    throw StorageError(StorageErrc::linkRefused, "A link leads out of the root");
                walk.directories.pop_back();
                walk.names.pop_back();
                continue;
            }
            auto next = detail::posix::openAt(walk.top(), name.c_str(), detail::posix::searchFlags | O_NOFOLLOW);
            if (!next) {
                const int   error = errno;
                struct stat status{};
                if (::fstatat(walk.top(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
                    if (S_ISLNK(status.st_mode)) {
                        spliceLink(walk, name, pending);
                        continue;
                    }
                    if (!S_ISDIR(status.st_mode))
                        throw StorageError(StorageErrc::notDirectory, "'" + name + "' is not a directory");
                }
                detail::posix::fail(error, "Cannot open '" + name + "'");
            }
            walk.names.push_back(name);
            auto path = location;
            for (const auto& component : walk.names)
                path /= component;
            checkTraversed(factsOf(detail::posix::statOf(next.get(), "Cannot inspect a directory"), path), detail::quoted(walk.names));
            walk.directories.push_back(std::move(next));
            notify(StoragePoint::componentOpened, walk.names.back());
        }
    }

    // The parent directory of a path and its final name.
    std::pair<Walk, std::string> resolveParent(const std::vector<std::string>& components) {
        auto                    walk = start();
        std::deque<std::string> pending(components.begin(), components.end());
        descend(walk, pending, 1);
        return {std::move(walk), std::move(pending.front())};
    }

    // Opens the final name of a resolution, following links inside the root when the policy allows.
    Fd openFinal(Walk& walk, std::string& name, int flags) {
        while (true) {
            auto file = detail::posix::openAt(walk.top(), name.c_str(), flags | O_NOFOLLOW);
            if (file)
                return file;
            const int   error = errno;
            struct stat status{};
            if ((error == ELOOP || error == EMLINK || error == ENOTDIR) && ::fstatat(walk.top(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(status.st_mode)) {
                std::deque<std::string> pending;
                spliceLink(walk, name, pending);
                if (pending.empty())
                    throw StorageError(StorageErrc::linkRefused, "The link '" + name + "' names a directory");
                descend(walk, pending, 1);
                name = std::move(pending.front());
                if (name == "." || name == "..")
                    throw StorageError(StorageErrc::linkRefused, "A link names a directory, not a file");
                continue;
            }
            detail::posix::fail(error, "Cannot open '" + name + "'");
        }
    }
};

class NativeStorage::Writer {
public:
    Writer(Writer&&) noexcept        = default;
    Writer& operator=(Writer&&)      = delete;  // would drop the temporary file of the writer it replaces
    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer() {
        abandon();
    }

    void write(std::span<const char> bytes) {
        if (!file)
            throw std::logic_error("The write is committed or abandoned");
        while (!bytes.empty()) {
            const auto written = ::write(file.get(), bytes.data(), bytes.size());
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0)
                detail::posix::fail(errno, "Cannot write " + detail::quoted(components));
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        }
    }

    void commit() {
        if (!file)
            throw std::logic_error("The write is committed or abandoned");
        if (::fsync(file.get()) != 0) {
            const int error = errno;
            abandon();
            detail::posix::fail(error, "Cannot write " + detail::quoted(components));
        }
        std::scoped_lock lock(state->structure);
        state->notify(StoragePoint::beforeCommit, name);
        try {
            state->confirm({walk.top()});
        } catch (...) {
            abandon();
            throw;
        }
        const int parent = walk.top();
        const int result = mode == WriteMode::replace ? ::renameat(parent, temporary.c_str(), parent, name.c_str())
                                                      : detail::posix::renameNoReplace(parent, temporary.c_str(), parent, name.c_str());
        if (result != 0) {
            const int error = errno;
            abandon();
            detail::posix::fail(error, "Cannot commit " + detail::quoted(components));
        }
        file.reset();
        state->confirm({parent});
    }

private:
    friend class NativeStorage;
    Writer(std::shared_ptr<State> state, State::Walk walk, std::vector<std::string> components, std::string name, WriteMode mode)
        : state(std::move(state)), walk(std::move(walk)), components(std::move(components)), name(std::move(name)), mode(mode) { }

    void abandon() noexcept {
        if (!file)
            return;
        file.reset();
        ::unlinkat(walk.top(), temporary.c_str(), 0);
    }

    std::shared_ptr<State>   state;
    State::Walk              walk;
    std::vector<std::string> components;
    std::string              name;
    WriteMode                mode;
    std::string              temporary;
    detail::posix::Fd        file;
};

inline NativeStorage::NativeStorage(const std::filesystem::path& location, RootOptions options, std::shared_ptr<const StorageHooks> hooks)
    : state(std::make_shared<State>()) {
    std::error_code error;
    auto            canonical = std::filesystem::canonical(location, error);
    if (error)
        throw StorageError(error == std::errc::no_such_file_or_directory ? StorageErrc::notFound : StorageErrc::ioError,
                           "Cannot declare the root: " + error.message());
    if (!std::filesystem::is_directory(canonical, error))
        throw StorageError(StorageErrc::notDirectory, "The root must be a directory");
    state->location = canonical;
    for (const auto& component : canonical.relative_path())
        state->locationComponents.push_back(component.string());
    state->options       = options;
    state->hooks         = std::move(hooks);
    state->effectiveUser = state->hooks && state->hooks->effectiveUser ? *state->hooks->effectiveUser : static_cast<std::uint32_t>(::geteuid());
    state->root          = state->openLocation(StorageErrc::notFound);
    state->rootIdentity  = detail::posix::identityOf(detail::posix::statOf(state->root.get(), "Cannot inspect the root"));
}

inline const std::filesystem::path& NativeStorage::location() const {
    return state->location;
}

inline DirectoryPage NativeStorage::list(std::string_view path, std::string_view after, std::size_t limit) {
    const auto              components = detail::storageComponents(path);
    auto                    walk       = state->start();
    std::deque<std::string> pending(components.begin(), components.end());
    state->descend(walk, pending, 0);
    state->notify(StoragePoint::resolved, components.empty() ? std::string_view{} : std::string_view(components.back()));
    detail::posix::Fd readable = detail::posix::openAt(walk.top(), ".", O_RDONLY | O_DIRECTORY);
    if (!readable)
        detail::posix::fail(errno, "Cannot list " + detail::quoted(components));
    DIR* directory = ::fdopendir(readable.get());
    if (!directory)
        detail::posix::fail(errno, "Cannot list " + detail::quoted(components));
    readable.release();
    std::vector<EntryInfo> entries;
    while (const auto* entry = ::readdir(directory)) {
        const std::string_view name = entry->d_name;
        if (name.starts_with(detail::temporaryPrefix) || !isSafeFileName(name))
            continue;
        struct stat status{};
        if (::fstatat(::dirfd(directory), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) == 0)
            entries.push_back(detail::posix::infoOf(std::string(name), status));
    }
    ::closedir(directory);
    state->confirm({walk.top()});
    return detail::pageOf(std::move(entries), after, limit);
}

inline EntryInfo NativeStorage::status(std::string_view path) {
    const auto components = detail::storageComponents(path);
    if (components.empty()) {
        auto walk = state->start();
        auto info = detail::posix::infoOf({}, detail::posix::statOf(walk.top(), "Cannot inspect the root"));
        state->confirm({walk.top()});
        return info;
    }
    auto [walk, name] = state->resolveParent(components);
    state->notify(StoragePoint::resolved, name);
    struct stat status{};
    if (::fstatat(walk.top(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
        detail::posix::fail(errno, "Cannot inspect " + detail::quoted(components));
    state->confirm({walk.top()});
    return detail::posix::infoOf(name, status);
}

inline File NativeStorage::openRead(std::string_view path) {
    const auto components = detail::entryComponents(path);
    auto [walk, name]     = state->resolveParent(components);
    state->notify(StoragePoint::resolved, name);
    auto       file   = state->openFinal(walk, name, O_RDONLY | O_NONBLOCK);
    const auto status = detail::posix::statOf(file.get(), "Cannot inspect " + detail::quoted(components));
    if (S_ISDIR(status.st_mode))
        throw StorageError(StorageErrc::isDirectory, detail::quoted(components) + " is a directory");
    if (!S_ISREG(status.st_mode))
        throw StorageError(StorageErrc::accessDenied, detail::quoted(components) + " is not a regular file");
    const int flags = ::fcntl(file.get(), F_GETFL);
    if (flags < 0 || ::fcntl(file.get(), F_SETFL, flags & ~O_NONBLOCK) != 0)
        detail::posix::fail(errno, "Cannot read " + detail::quoted(components));
    state->confirm({walk.top()});
    return File{std::make_unique<detail::posix::FdSource>(std::move(file), static_cast<std::uint64_t>(status.st_size))};
}

inline NativeStorage::Writer NativeStorage::beginWrite(std::string_view path, WriteMode mode) {
    auto components   = detail::entryComponents(path);
    auto [walk, name] = state->resolveParent(components);
    state->notify(StoragePoint::resolved, name);
    struct stat status{};
    if (::fstatat(walk.top(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
        if (mode == WriteMode::createNew)
            throw StorageError(StorageErrc::alreadyExists, detail::quoted(components) + " already exists");
        if (S_ISDIR(status.st_mode))
            throw StorageError(StorageErrc::isDirectory, detail::quoted(components) + " is a directory");
    }
    Writer writer{state, std::move(walk), std::move(components), std::move(name), mode};
    for (int attempt = 0; !writer.file; ++attempt) {
        writer.temporary = detail::temporaryName();
        writer.file      = detail::posix::openAt(writer.walk.top(), writer.temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
        if (!writer.file && (errno != EEXIST || attempt == 8))
            detail::posix::fail(errno, "Cannot write " + detail::quoted(writer.components));
    }
    state->notify(StoragePoint::temporaryCreated, writer.name);
    return writer;
}

inline void NativeStorage::createDirectory(std::string_view path) {
    const auto components = detail::entryComponents(path);
    auto [walk, name]     = state->resolveParent(components);
    std::scoped_lock lock(state->structure);
    state->notify(StoragePoint::resolved, name);
    if (::mkdirat(walk.top(), name.c_str(), 0755) != 0)
        detail::posix::fail(errno, "Cannot create " + detail::quoted(components));
    state->confirm({walk.top()});
}

inline void NativeStorage::remove(std::string_view path) {
    const auto components = detail::entryComponents(path);
    auto [walk, name]     = state->resolveParent(components);
    std::scoped_lock lock(state->structure);
    state->notify(StoragePoint::resolved, name);
    struct stat status{};
    if (::fstatat(walk.top(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
        detail::posix::fail(errno, "Cannot remove " + detail::quoted(components));
    const int flags = S_ISDIR(status.st_mode) ? AT_REMOVEDIR : 0;
    if (::unlinkat(walk.top(), name.c_str(), flags) != 0) {
        const int error = errno;
        // The entry changed type since it was inspected: remove what it is now, still relative to its directory.
        if ((error == EISDIR || error == ENOTDIR || error == EPERM) && ::unlinkat(walk.top(), name.c_str(), flags ^ AT_REMOVEDIR) == 0) {
            state->confirm({walk.top()});
            return;
        }
        detail::posix::fail(error == EEXIST ? ENOTEMPTY : error, "Cannot remove " + detail::quoted(components));
    }
    state->confirm({walk.top()});
}

inline void NativeStorage::rename(std::string_view from, std::string_view to) {
    const auto source             = detail::entryComponents(from);
    const auto target             = detail::entryComponents(to);
    auto [sourceWalk, sourceName] = state->resolveParent(source);
    auto [targetWalk, targetName] = state->resolveParent(target);
    std::scoped_lock lock(state->structure);
    state->notify(StoragePoint::resolved, sourceName);
    if (detail::posix::renameNoReplace(sourceWalk.top(), sourceName.c_str(), targetWalk.top(), targetName.c_str()) != 0)
        detail::posix::fail(errno, "Cannot rename " + detail::quoted(source));
    state->confirm({sourceWalk.top(), targetWalk.top()});
}

#else  // _WIN32

namespace detail::windows {

    #ifndef FILE_OPEN
        #define FILE_OPEN 0x00000001
    #endif
    #ifndef FILE_CREATE
        #define FILE_CREATE 0x00000002
    #endif
    #ifndef FILE_DIRECTORY_FILE
        #define FILE_DIRECTORY_FILE 0x00000001
    #endif
    #ifndef FILE_NON_DIRECTORY_FILE
        #define FILE_NON_DIRECTORY_FILE 0x00000040
    #endif
    #ifndef FILE_SYNCHRONOUS_IO_NONALERT
        #define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
    #endif
    #ifndef FILE_OPEN_REPARSE_POINT
        #define FILE_OPEN_REPARSE_POINT 0x00200000
    #endif
    #ifndef OBJ_CASE_INSENSITIVE
        #define OBJ_CASE_INSENSITIVE 0x00000040L
    #endif

inline constexpr NTSTATUS statusObjectNameNotFound  = static_cast<NTSTATUS>(0xC0000034L);
inline constexpr NTSTATUS statusObjectPathNotFound  = static_cast<NTSTATUS>(0xC000003AL);
inline constexpr NTSTATUS statusObjectNameCollision = static_cast<NTSTATUS>(0xC0000035L);
inline constexpr NTSTATUS statusObjectNameInvalid   = static_cast<NTSTATUS>(0xC0000033L);
inline constexpr NTSTATUS statusAccessDenied        = static_cast<NTSTATUS>(0xC0000022L);
inline constexpr NTSTATUS statusSharingViolation    = static_cast<NTSTATUS>(0xC0000043L);
inline constexpr NTSTATUS statusDeletePending       = static_cast<NTSTATUS>(0xC0000056L);
inline constexpr NTSTATUS statusNotADirectory       = static_cast<NTSTATUS>(0xC0000103L);
inline constexpr NTSTATUS statusFileIsADirectory    = static_cast<NTSTATUS>(0xC00000BAL);
inline constexpr NTSTATUS statusDirectoryNotEmpty   = static_cast<NTSTATUS>(0xC0000101L);

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle) { }
    Handle(Handle&& other) noexcept : handle(std::exchange(other.handle, nullptr)) { }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        reset();
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle != nullptr;
    }
    void reset() noexcept {
        if (handle)
            ::CloseHandle(handle);
        handle = nullptr;
    }

private:
    HANDLE handle{nullptr};
};

struct Identity {
    ULONGLONG            volume{};
    std::array<BYTE, 16> file{};
    bool                 operator==(const Identity&) const = default;
};

inline StorageErrc errcOf(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return StorageErrc::notFound;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return StorageErrc::alreadyExists;
        case ERROR_DIRECTORY:
            return StorageErrc::notDirectory;
        case ERROR_DIR_NOT_EMPTY:
            return StorageErrc::notEmpty;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
            return StorageErrc::accessDenied;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return StorageErrc::busy;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return StorageErrc::invalidPath;
        case ERROR_CANT_ACCESS_FILE:
        case ERROR_STOPPED_ON_SYMLINK:
            return StorageErrc::linkRefused;
        default:
            return StorageErrc::ioError;
    }
}

[[noreturn]] inline void fail(DWORD error, const std::string& what) {
    throw StorageError(errcOf(error), what + ": " + std::system_category().message(static_cast<int>(error)));
}

[[noreturn]] inline void failLast(const std::string& what) {
    fail(::GetLastError(), what);
}

// The NT functions WebFront needs from ntdll.dll, which every process has loaded.
struct Ntdll {
    // Not named CreateFile: <windows.h> defines that name as a macro.
    using CreateFunction =
        NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using ToDosFunction = ULONG(NTAPI*)(NTSTATUS);

    CreateFunction createFile{};
    ToDosFunction  toDosError{};

    static const Ntdll& get() {
        static const Ntdll functions = [] {
            Ntdll      found;
            const auto library = ::GetModuleHandleW(L"ntdll.dll");
            if (library) {
                found.createFile = reinterpret_cast<CreateFunction>(reinterpret_cast<void*>(::GetProcAddress(library, "NtCreateFile")));
                found.toDosError = reinterpret_cast<ToDosFunction>(reinterpret_cast<void*>(::GetProcAddress(library, "RtlNtStatusToDosError")));
            }
            return found;
        }();
        if (!functions.createFile || !functions.toDosError)
            throw StorageError(StorageErrc::ioError, "ntdll.dll does not provide NtCreateFile");
        return functions;
    }
};

[[noreturn]] inline void failStatus(NTSTATUS status, const std::string& what) {
    switch (status) {
        case statusObjectNameNotFound:
        case statusObjectPathNotFound:
        case statusDeletePending:
            throw StorageError(StorageErrc::notFound, what + ": no such entry");
        case statusObjectNameCollision:
            throw StorageError(StorageErrc::alreadyExists, what + ": the name exists");
        case statusObjectNameInvalid:
            throw StorageError(StorageErrc::invalidPath, what + ": invalid name");
        case statusAccessDenied:
            throw StorageError(StorageErrc::accessDenied, what + ": access denied");
        case statusSharingViolation:
            throw StorageError(StorageErrc::busy, what + ": the entry is in use");
        case statusNotADirectory:
            throw StorageError(StorageErrc::notDirectory, what + ": not a directory");
        case statusFileIsADirectory:
            throw StorageError(StorageErrc::isDirectory, what + ": a directory");
        case statusDirectoryNotEmpty:
            throw StorageError(StorageErrc::notEmpty, what + ": the directory is not empty");
        default:
            fail(Ntdll::get().toDosError(status), what);
    }
}

inline std::wstring wide(std::string_view utf8) {
    return detail::fromUtf8(utf8).wstring();
}

inline std::string utf8(std::wstring_view text) {
    const auto bytes = std::filesystem::path(text).u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Opens `name` relative to the directory `parent`, never following a reparse point (link or junction).
inline NTSTATUS openRelative(Handle& result, HANDLE parent, const std::wstring& name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options) {
    if (name.size() > 32767)
        return statusObjectNameInvalid;
    UNICODE_STRING unicode{};
    unicode.Buffer        = const_cast<PWSTR>(name.c_str());
    unicode.Length        = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode.MaximumLength = unicode.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length        = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName    = &unicode;
    attributes.Attributes    = OBJ_CASE_INSENSITIVE;
    IO_STATUS_BLOCK io{};
    HANDLE          handle = nullptr;
    const auto      status = Ntdll::get().createFile(&handle,
                                                     access | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                                                     &attributes,
                                                     &io,
                                                     nullptr,
                                                     FILE_ATTRIBUTE_NORMAL,
                                                     share,
                                                     disposition,
                                                     options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT,
                                                     nullptr,
                                                     0);
    if (status >= 0)
        result = Handle{handle};
    return status;
}

inline DWORD attributesOf(HANDLE handle, const std::string& what) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
        failLast(what);
    return info.FileAttributes;
}

inline Identity identityOf(HANDLE handle) {
    FILE_ID_INFO info{};
    if (!::GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)))
        failLast("Cannot identify the root");
    Identity identity{info.VolumeSerialNumber, {}};
    std::copy(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), identity.file.begin());
    return identity;
}

inline std::chrono::system_clock::time_point timeOf(LONGLONG fileTime) {
    // A FILETIME counts 100 ns intervals since 1601-01-01.
    constexpr LONGLONG unixEpoch = 116444736000000000LL;
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>{fileTime - unixEpoch})};
}

inline EntryInfo infoOf(std::string name, DWORD attributes, LONGLONG size, LONGLONG modified) {
    EntryInfo info{.name = std::move(name), .modified = timeOf(modified), .readable = true, .writable = (attributes & FILE_ATTRIBUTE_READONLY) == 0};
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        info.type = EntryType::link;
    else if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        info.type = EntryType::directory;
    else {
        info.type = EntryType::file;
        info.size = static_cast<std::uint64_t>(size);
    }
    return info;
}

inline EntryInfo infoOf(std::string name, HANDLE handle, const std::string& what) {
    FILE_BASIC_INFO    basic{};
    FILE_STANDARD_INFO standard{};
    if (!::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || !::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)))
        failLast(what);
    return infoOf(std::move(name), basic.FileAttributes, standard.EndOfFile.QuadPart, basic.LastWriteTime.QuadPart);
}

// Renames the entry of `handle` to `name` in the directory `directory`.
inline bool renameHandle(HANDLE handle, HANDLE directory, const std::wstring& name, bool replace) {
    const auto                 nameBytes = name.size() * sizeof(wchar_t);
    std::vector<std::uint64_t> buffer((sizeof(FILE_RENAME_INFO) + nameBytes + sizeof(std::uint64_t)) / sizeof(std::uint64_t) + 1);
    auto*                      info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists           = replace ? TRUE : FALSE;
    info->RootDirectory             = directory;
    info->FileNameLength            = static_cast<DWORD>(nameBytes);
    std::memcpy(reinterpret_cast<std::byte*>(buffer.data()) + offsetof(FILE_RENAME_INFO, FileName), name.data(), nameBytes);
    return ::SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t))) != FALSE;
}

inline bool markDeleted(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return ::SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}

inline std::wstring finalPath(HANDLE handle) {
    std::wstring path(512, L'\0');
    while (true) {
        const auto length = ::GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
        if (length == 0)
            return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(length + 1);
    }
}

// The content of a file opened relative to a directory of the root.
class HandleSource : public ByteSource {
public:
    HandleSource(Handle file, std::uint64_t total) : file(std::move(file)), total(total) { }

    std::size_t read(std::span<char> buffer) override {
        std::size_t count = 0;
        while (count < buffer.size()) {
            DWORD      got  = 0;
            const auto want = static_cast<DWORD>(std::min<std::size_t>(buffer.size() - count, 1u << 30));
            if (!::ReadFile(file.get(), buffer.data() + count, want, &got, nullptr))
                failLast("Cannot read the file");
            if (got == 0)
                break;
            count += got;
        }
        return count;
    }
    [[nodiscard]] std::optional<std::uint64_t> size() const override {
        return total;
    }
    [[nodiscard]] bool seek(std::uint64_t offset) override {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(std::min(offset, total));
        return ::SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN) != FALSE;
    }

private:
    Handle        file;
    std::uint64_t total;
};

inline constexpr ACCESS_MASK directoryAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE;
// The directories of an operation are held without FILE_SHARE_DELETE: nobody can rename or remove them meanwhile.
inline constexpr ULONG heldSharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
inline constexpr ULONG fullSharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

}  // namespace detail::windows

struct NativeStorage::State {
    using Handle   = detail::windows::Handle;
    using Identity = detail::windows::Identity;

    std::filesystem::path               location;
    std::wstring                        volume;
    std::vector<std::string>            locationComponents;
    RootOptions                         options;
    std::shared_ptr<const StorageHooks> hooks;
    Handle                              root;
    Identity                            rootIdentity;
    std::wstring                        rootPath;
    std::mutex                          structure;
    std::atomic<bool>                   moved{false};

    struct Walk {
        std::vector<Handle>      directories;
        std::vector<std::string> names;

        [[nodiscard]] HANDLE top() const {
            return directories.back().get();
        }
    };

    void notify(StoragePoint point, std::string_view name) const {
        if (hooks && hooks->at)
            hooks->at(point, name);
    }

    // Opens a directory relative to `parent`, refusing reparse points.
    static Handle
    openDirectory(HANDLE parent, const std::string& name, ULONG sharing, const std::string& what, ACCESS_MASK access = detail::windows::directoryAccess) {
        Handle     directory;
        const auto status = detail::windows::openRelative(directory, parent, detail::windows::wide(name), access, sharing, FILE_OPEN, FILE_DIRECTORY_FILE);
        if (status == detail::windows::statusNotADirectory) {
            Handle entry;
            if (detail::windows::openRelative(entry, parent, detail::windows::wide(name), 0, detail::windows::fullSharing, FILE_OPEN, 0) >= 0
                && (detail::windows::attributesOf(entry.get(), what) & FILE_ATTRIBUTE_REPARSE_POINT))
                throw StorageError(StorageErrc::linkRefused, what + " is a link");
        }
        if (status < 0)
            detail::windows::failStatus(status, what);
        if (detail::windows::attributesOf(directory.get(), what) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw StorageError(StorageErrc::linkRefused, what + " is a link");
        return directory;
    }

    // Opens the declared location from its volume, refusing reparse points on the way.
    Handle openLocation(StorageErrc onMove, ULONG rootSharing) const {
        Handle current{::CreateFileW(volume.c_str(),
                                     detail::windows::directoryAccess | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                     detail::windows::fullSharing,
                                     nullptr,
                                     OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr)};
        if (!current)
            detail::windows::failLast("Cannot open the volume of the root");
        for (std::size_t index = 0; index < locationComponents.size(); ++index) {
            const bool last = index + 1 == locationComponents.size();
            try {
                // An ancestor is only traversed; the root is listed.
                current = last ? openDirectory(current.get(), locationComponents[index], rootSharing, "The location")
                               : openDirectory(current.get(), locationComponents[index], detail::windows::fullSharing, "The location", FILE_TRAVERSE);
            } catch (const StorageError& error) {
                if (error.errc() == StorageErrc::accessDenied || error.errc() == StorageErrc::busy)
                    throw;
                throw StorageError(onMove, "The root is no longer at its declared location");
            }
        }
        return current;
    }

    void verifyLocation() {
        if (moved)
            throw StorageError(StorageErrc::rootMoved, "The root is no longer at its declared location");
        try {
            const auto found = openLocation(StorageErrc::rootMoved, detail::windows::fullSharing);
            if (detail::windows::identityOf(found.get()) != rootIdentity)
                throw StorageError(StorageErrc::rootMoved, "The root is no longer at its declared location");
        } catch (const StorageError& error) {
            if (error.errc() == StorageErrc::rootMoved)
                moved = true;
            throw;
        }
    }

    // The final path of a directory begins with the root's: its handles prevent the moves, this checks it still holds.
    bool isUnderRoot(HANDLE directory) const {
        const auto path = detail::windows::finalPath(directory);
        if (path.empty() || rootPath.empty() || path.size() < rootPath.size())
            return false;
        if (::CompareStringOrdinal(path.data(), static_cast<int>(rootPath.size()), rootPath.data(), static_cast<int>(rootPath.size()), TRUE) != CSTR_EQUAL)
            return false;
        return path.size() == rootPath.size() || path[rootPath.size()] == L'\\';
    }

    void confirm(std::initializer_list<HANDLE> directories) {
        for (const auto directory : directories)
            if (!isUnderRoot(directory))
                throw StorageError(StorageErrc::confinementViolated, "A directory left the root during the operation");
        try {
            verifyLocation();
        } catch (const StorageError& error) {
            if (error.errc() != StorageErrc::rootMoved)
                throw;
            throw StorageError(StorageErrc::confinementViolated, "The root moved during the operation");
        }
    }

    Walk start() {
        verifyLocation();
        Walk   walk;
        Handle top{::ReOpenFile(root.get(),
                                detail::windows::directoryAccess | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                detail::windows::heldSharing,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)};
        if (!top)
            detail::windows::failLast("Cannot open the root");
        walk.directories.push_back(std::move(top));
        return walk;
    }

    void descend(Walk& walk, const std::vector<std::string>& components, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            walk.names.push_back(components[index]);
            walk.directories.push_back(openDirectory(walk.top(), components[index], detail::windows::heldSharing, detail::quoted(walk.names)));
            notify(StoragePoint::componentOpened, components[index]);
        }
    }

    std::pair<Walk, std::string> resolveParent(const std::vector<std::string>& components) {
        auto walk = start();
        descend(walk, components, components.size() - 1);
        return {std::move(walk), components.back()};
    }

    // Opens an entry of the top directory, refusing a reparse point.
    static Handle openEntry(const Walk& walk, const std::string& name, ACCESS_MASK access, ULONG sharing, ULONG options, const std::string& what) {
        Handle     entry;
        const auto status = detail::windows::openRelative(entry, walk.top(), detail::windows::wide(name), access, sharing, FILE_OPEN, options);
        if (status < 0)
            detail::windows::failStatus(status, what);
        if (detail::windows::attributesOf(entry.get(), what) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw StorageError(StorageErrc::linkRefused, what + " is a link");
        return entry;
    }
};

class NativeStorage::Writer {
public:
    Writer(Writer&&) noexcept        = default;
    Writer& operator=(Writer&&)      = delete;  // would drop the temporary file of the writer it replaces
    Writer(const Writer&)            = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer() {
        abandon();
    }

    void write(std::span<const char> bytes) {
        if (!file)
            throw std::logic_error("The write is committed or abandoned");
        while (!bytes.empty()) {
            DWORD      written = 0;
            const auto want    = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1u << 30));
            if (!::WriteFile(file.get(), bytes.data(), want, &written, nullptr))
                detail::windows::failLast("Cannot write " + detail::quoted(components));
            bytes = bytes.subspan(written);
        }
    }

    void commit() {
        if (!file)
            throw std::logic_error("The write is committed or abandoned");
        if (!::FlushFileBuffers(file.get())) {
            const auto error = ::GetLastError();
            abandon();
            detail::windows::fail(error, "Cannot write " + detail::quoted(components));
        }
        std::scoped_lock lock(state->structure);
        state->notify(StoragePoint::beforeCommit, name);
        try {
            state->confirm({walk.top()});
        } catch (...) {
            abandon();
            throw;
        }
        if (!detail::windows::renameHandle(file.get(), walk.top(), detail::windows::wide(name), mode == WriteMode::replace)) {
            const auto error = ::GetLastError();
            abandon();
            if (error == ERROR_ACCESS_DENIED && mode == WriteMode::replace)
                throw StorageError(StorageErrc::accessDenied, "Cannot replace " + detail::quoted(components));
            detail::windows::fail(error, "Cannot commit " + detail::quoted(components));
        }
        file.reset();
        state->confirm({walk.top()});
    }

private:
    friend class NativeStorage;
    Writer(std::shared_ptr<State> state, State::Walk walk, std::vector<std::string> components, std::string name, WriteMode mode)
        : state(std::move(state)), walk(std::move(walk)), components(std::move(components)), name(std::move(name)), mode(mode) { }

    void abandon() noexcept {
        if (!file)
            return;
        detail::windows::markDeleted(file.get());
        file.reset();
    }

    std::shared_ptr<State>   state;
    State::Walk              walk;
    std::vector<std::string> components;
    std::string              name;
    WriteMode                mode;
    detail::windows::Handle  file;
};

inline NativeStorage::NativeStorage(const std::filesystem::path& location, RootOptions options, std::shared_ptr<const StorageHooks> hooks)
    : state(std::make_shared<State>()) {
    std::error_code error;
    auto            canonical = std::filesystem::canonical(location, error);
    if (error)
        throw StorageError(error == std::errc::no_such_file_or_directory ? StorageErrc::notFound : StorageErrc::ioError,
                           "Cannot declare the root: " + error.message());
    if (!std::filesystem::is_directory(canonical, error))
        throw StorageError(StorageErrc::notDirectory, "The root must be a directory");
    state->location = canonical;
    const auto name = canonical.root_name().wstring();
    state->volume   = name.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + name.substr(2) + L"\\" : L"\\\\?\\" + name + L"\\";
    for (const auto& component : canonical.relative_path()) {
        const auto bytes = component.u8string();
        state->locationComponents.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    state->options      = options;
    state->hooks        = std::move(hooks);
    state->root         = state->openLocation(StorageErrc::notFound, detail::windows::heldSharing);
    state->rootIdentity = detail::windows::identityOf(state->root.get());
    state->rootPath     = detail::windows::finalPath(state->root.get());
}

inline const std::filesystem::path& NativeStorage::location() const {
    return state->location;
}

inline DirectoryPage NativeStorage::list(std::string_view path, std::string_view after, std::size_t limit) {
    const auto components = detail::storageComponents(path);
    auto       walk       = state->start();
    state->descend(walk, components, components.size());
    state->notify(StoragePoint::resolved, components.empty() ? std::string_view{} : std::string_view(components.back()));
    std::vector<EntryInfo>     entries;
    std::vector<std::uint64_t> buffer(64 * 1024 / sizeof(std::uint64_t));
    auto                       kind = FileIdBothDirectoryRestartInfo;
    while (::GetFileInformationByHandleEx(walk.top(), kind, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t)))) {
        kind              = FileIdBothDirectoryInfo;
        const auto* bytes = reinterpret_cast<const std::byte*>(buffer.data());
        while (true) {
            const auto*        entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(bytes);
            const std::wstring wideName(reinterpret_cast<const wchar_t*>(bytes + offsetof(FILE_ID_BOTH_DIR_INFO, FileName)),
                                        entry->FileNameLength / sizeof(wchar_t));
            const auto         name = detail::windows::utf8(wideName);
            if (name != "." && name != ".." && !std::string_view(name).starts_with(detail::temporaryPrefix) && isSafeFileName(name))
                entries.push_back(detail::windows::infoOf(name, entry->FileAttributes, entry->EndOfFile.QuadPart, entry->LastWriteTime.QuadPart));
            if (entry->NextEntryOffset == 0)
                break;
            bytes += entry->NextEntryOffset;
        }
    }
    if (const auto error = ::GetLastError(); error != ERROR_NO_MORE_FILES)
        detail::windows::fail(error, "Cannot list " + detail::quoted(components));
    state->confirm({walk.top()});
    return detail::pageOf(std::move(entries), after, limit);
}

inline EntryInfo NativeStorage::status(std::string_view path) {
    const auto components = detail::storageComponents(path);
    if (components.empty()) {
        auto walk = state->start();
        auto info = detail::windows::infoOf({}, walk.top(), "Cannot inspect the root");
        state->confirm({walk.top()});
        return info;
    }
    auto [walk, name] = state->resolveParent(components);
    state->notify(StoragePoint::resolved, name);
    detail::windows::Handle entry;
    const auto status = detail::windows::openRelative(entry, walk.top(), detail::windows::wide(name), 0, detail::windows::fullSharing, FILE_OPEN, 0);
    if (status < 0)
        detail::windows::failStatus(status, "Cannot inspect " + detail::quoted(components));
    auto info = detail::windows::infoOf(name, entry.get(), "Cannot inspect " + detail::quoted(components));
    state->confirm({walk.top()});
    return info;
}

inline File NativeStorage::openRead(std::string_view path) {
    const auto components = detail::entryComponents(path);
    auto [walk, name]     = state->resolveParent(components);
    state->notify(StoragePoint::resolved, name);
    const auto what = detail::quoted(components);
    auto       file = State::openEntry(walk, name, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE, 0, what);
    if (detail::windows::attributesOf(file.get(), what) & FILE_ATTRIBUTE_DIRECTORY)
        throw StorageError(StorageErrc::isDirectory, what + " is a directory");
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size))
        detail::windows::failLast("Cannot read " + what);
    state->confirm({walk.top()});
    return File{std::make_unique<detail::windows::HandleSource>(std::move(file), static_cast<std::uint64_t>(size.QuadPart))};
}

inline NativeStorage::Writer NativeStorage::beginWrite(std::string_view path, WriteMode mode) {
    auto components   = detail::entryComponents(path);
    auto [walk, name] = state->resolveParent(components);
    state->notify(StoragePoint::resolved, name);
    detail::windows::Handle existing;
    if (detail::windows::openRelative(existing, walk.top(), detail::windows::wide(name), 0, detail::windows::fullSharing, FILE_OPEN, 0) >= 0) {
        if (mode == WriteMode::createNew)
            throw StorageError(StorageErrc::alreadyExists, detail::quoted(components) + " already exists");
        const auto attributes = detail::windows::attributesOf(existing.get(), detail::quoted(components));
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw StorageError(StorageErrc::isDirectory, detail::quoted(components) + " is a directory");
        existing.reset();
    }
    Writer writer{state, std::move(walk), std::move(components), std::move(name), mode};
    for (int attempt = 0; !writer.file; ++attempt) {
        const auto status = detail::windows::openRelative(writer.file,
                                                          writer.walk.top(),
                                                          detail::windows::wide(detail::temporaryName()),
                                                          FILE_WRITE_DATA | DELETE,
                                                          0,
                                                          FILE_CREATE,
                                                          FILE_NON_DIRECTORY_FILE);
        if (status < 0 && (status != detail::windows::statusObjectNameCollision || attempt == 8))
            detail::windows::failStatus(status, "Cannot write " + detail::quoted(writer.components));
    }
    state->notify(StoragePoint::temporaryCreated, writer.name);
    return writer;
}

inline void NativeStorage::createDirectory(std::string_view path) {
    const auto components = detail::entryComponents(path);
    auto [walk, name]     = state->resolveParent(components);
    std::scoped_lock lock(state->structure);
    state->notify(StoragePoint::resolved, name);
    detail::windows::Handle directory;
    const auto              status = detail::windows::openRelative(directory,
                                                                   walk.top(),
                                                                   detail::windows::wide(name),
                                                                   detail::windows::directoryAccess,
                                                                   detail::windows::fullSharing,
                                                                   FILE_CREATE,
                                                                   FILE_DIRECTORY_FILE);
    if (status < 0)
        detail::windows::failStatus(status, "Cannot create " + detail::quoted(components));
    directory.reset();
    state->confirm({walk.top()});
}

inline void NativeStorage::remove(std::string_view path) {
    const auto components = detail::entryComponents(path);
    auto [walk, name]     = state->resolveParent(components);
    std::scoped_lock lock(state->structure);
    state->notify(StoragePoint::resolved, name);
    detail::windows::Handle entry;
    const auto status = detail::windows::openRelative(entry, walk.top(), detail::windows::wide(name), DELETE, detail::windows::fullSharing, FILE_OPEN, 0);
    if (status < 0)
        detail::windows::failStatus(status, "Cannot remove " + detail::quoted(components));
    if (!detail::windows::markDeleted(entry.get()))
        detail::windows::failLast("Cannot remove " + detail::quoted(components));
    entry.reset();
    state->confirm({walk.top()});
}

inline void NativeStorage::rename(std::string_view from, std::string_view to) {
    const auto source             = detail::entryComponents(from);
    const auto target             = detail::entryComponents(to);
    auto [sourceWalk, sourceName] = state->resolveParent(source);
    auto [targetWalk, targetName] = state->resolveParent(target);
    std::scoped_lock lock(state->structure);
    state->notify(StoragePoint::resolved, sourceName);
    detail::windows::Handle entry;
    const auto              status =
        detail::windows::openRelative(entry, sourceWalk.top(), detail::windows::wide(sourceName), DELETE, detail::windows::fullSharing, FILE_OPEN, 0);
    if (status < 0)
        detail::windows::failStatus(status, "Cannot rename " + detail::quoted(source));
    if (!detail::windows::renameHandle(entry.get(), targetWalk.top(), detail::windows::wide(targetName), false))
        detail::windows::failLast("Cannot rename " + detail::quoted(source));
    entry.reset();
    state->confirm({sourceWalk.top(), targetWalk.top()});
}

#endif  // _WIN32

static_assert(Storage<NativeStorage>);

}  // namespace webfront::fs
