#pragma once

/// A browser profile directory that persists across runs but is never used by two live processes at once.
///
/// Chromium (CEF) hands a second process that opens the same profile over to the first one and exits, and WebKit's databases
/// are not meant for concurrent writers. With a lock file held for the life of the process, the first instance keeps the
/// directory (so a fixed port keeps its storage from one run to the next) and every other live instance of the same
/// executable gets a directory of its own, in a sibling `<directory>.instances`, removed when it ends. The directories of
/// instances that died without cleaning up are removed by the next lease. WebView2 shares one user data folder between
/// processes by design, and a WKWebView data store is not persistent, so neither needs this.

#include <cerrno>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <system_error>
#include <vector>

#ifndef _WIN32
    #include <fcntl.h>
    #include <sys/file.h>
    #include <unistd.h>
#endif

namespace webfront::frontend {

#ifndef _WIN32
namespace detail {

/// An exclusive flock on a file: held until destruction (or the death of the process, which the system releases).
class FileLock {
public:
    FileLock() = default;
    FileLock(const FileLock&)            = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    ~FileLock() {
        if (fd >= 0)
            ::close(fd);
    }

    /// Takes the lock of `file`, creating it. Returns nullopt when another process holds it (contention, the only expected
    /// refusal) and throws std::system_error for any other failure (no descriptor left, a file that cannot be created...).
    [[nodiscard]] static std::optional<FileLock> tryLock(const std::filesystem::path& file) { return lock(file, LOCK_EX | LOCK_NB); }

    /// Waits for the lock of `file`; used for the short critical section that coordinates the leases of a directory.
    [[nodiscard]] static FileLock wait(const std::filesystem::path& file) { return std::move(*lock(file, LOCK_EX)); }

private:
    static std::optional<FileLock> lock(const std::filesystem::path& file, int operation) {
        FileLock result;
        result.fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (result.fd < 0)
            throw std::system_error(errno, std::generic_category(), "profile lock " + file.string());
        while (::flock(result.fd, operation) != 0) {
            if (errno == EINTR)
                continue;
            if (errno == EWOULDBLOCK)
                return std::nullopt;
            throw std::system_error(errno, std::generic_category(), "profile lock " + file.string());
        }
        return result;
    }

    int fd = -1;
};

inline constexpr const char* leaseFile    = ".webfront-lock";
inline constexpr const char* registryFile = ".registry";

/// What a process keeps of its lease until it exits: the lock, and the directory to remove when it is not the shared one.
struct LeaseHolder {
    std::vector<std::pair<FileLock, std::filesystem::path>> leases;
    ~LeaseHolder() {
        for (auto& [lock, remove] : leases) {
            std::error_code ignored;
            if (!remove.empty())
                std::filesystem::remove_all(remove, ignored);
        }
    }
};

inline LeaseHolder& leaseHolder() {
    static LeaseHolder holder;
    return holder;
}

}  // namespace detail

/// The directory to use as a profile: `directory` for the first live process, a directory of its own for the others.
/// Throws std::system_error when a lock cannot be taken for a reason other than another process holding it.
inline std::filesystem::path leaseProfileDirectory(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    auto&      holder    = detail::leaseHolder();
    const auto instances = std::filesystem::path(directory.string() + ".instances");
    // Creating an instance directory and locking it, and removing the abandoned ones, never interleave between processes: a
    // directory that exists without its lock held yet is a starting process, not an abandoned one. The registry lock is held
    // only for these steps, and a process that dies releases it.
    const auto removeAbandoned = [&] {
        for (const auto& entry : std::filesystem::directory_iterator(instances)) {
            if (!entry.is_directory())
                continue;
            if (auto stale = detail::FileLock::tryLock(entry.path() / detail::leaseFile)) {
                std::error_code ignored;
                std::filesystem::remove_all(entry.path(), ignored);
            }
        }
    };
    if (auto shared = detail::FileLock::tryLock(directory / detail::leaseFile)) {
        holder.leases.emplace_back(std::move(*shared), std::filesystem::path{});
        // The instances of an earlier run may have died after the shared profile's owner ended: nobody else would clear them.
        if (std::filesystem::exists(instances)) {
            const auto registry = detail::FileLock::wait(instances / detail::registryFile);
            removeAbandoned();
        }
        return directory;
    }
    std::filesystem::create_directories(instances);
    const auto registry = detail::FileLock::wait(instances / detail::registryFile);
    removeAbandoned();
    // Another process with the same number (another PID namespace over the same directory) makes a name busy: the next one.
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        const auto candidate = instances / (std::to_string(::getpid()) + "-" + std::to_string(attempt));
        std::filesystem::create_directories(candidate);
        if (auto own = detail::FileLock::tryLock(candidate / detail::leaseFile)) {
            holder.leases.emplace_back(std::move(*own), candidate);
            return candidate;
        }
    }
    throw std::runtime_error("no profile directory is free in " + instances.string());
}
#else
inline std::filesystem::path leaseProfileDirectory(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    return directory;
}
#endif

}  // namespace webfront::frontend
