#pragma once

/// A browser profile directory that persists across runs but is never used by two live processes at once.
///
/// Chromium (CEF) hands a second process that opens the same profile over to the first one and exits, and WebKit's databases
/// are not meant for concurrent writers. With a lock file held for the life of the process, the first instance keeps the
/// directory (so a fixed port keeps its storage from one run to the next) and every other live instance of the same
/// executable gets a directory of its own, in a sibling `<directory>.instances`, removed when it ends. The directories of
/// instances that died without cleaning up are removed by the next lease. WebView2 shares one user data folder between
/// processes by design, and a WKWebView data store is not persistent, so neither needs this.

#include <filesystem>
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
    explicit FileLock(const std::filesystem::path& file) {
        fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
        }
    }
    FileLock(const FileLock&)            = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    ~FileLock() {
        if (fd >= 0)
            ::close(fd);
    }
    [[nodiscard]] bool held() const noexcept { return fd >= 0; }

private:
    int fd = -1;
};

inline constexpr const char* leaseFile = ".webfront-lock";

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

inline std::filesystem::path leaseProfileDirectory(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    auto& holder = detail::leaseHolder();
    if (detail::FileLock shared{directory / detail::leaseFile}; shared.held()) {
        holder.leases.emplace_back(std::move(shared), std::filesystem::path{});
        return directory;
    }
    const auto           instances = std::filesystem::path(directory.string() + ".instances");
    std::error_code      error;
    std::filesystem::create_directories(instances, error);
    // The directories whose owner is gone: nobody holds their lock any more.
    for (const auto& entry : std::filesystem::directory_iterator(instances, error)) {
        if (!entry.is_directory(error))
            continue;
        if (detail::FileLock stale{entry.path() / detail::leaseFile}; stale.held())
            std::filesystem::remove_all(entry.path(), error);
    }
    for (unsigned attempt = 0;; ++attempt) {
        const auto candidate = instances / (std::to_string(::getpid()) + "-" + std::to_string(attempt));
        std::filesystem::create_directories(candidate);
        if (detail::FileLock own{candidate / detail::leaseFile}; own.held()) {
            holder.leases.emplace_back(std::move(own), candidate);
            return candidate;
        }
    }
}
#else
inline std::filesystem::path leaseProfileDirectory(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    return directory;
}
#endif

}  // namespace webfront::frontend
