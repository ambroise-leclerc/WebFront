#pragma once

#if !defined(__linux__) && !defined(__APPLE__)
    #error "WebFront Unix PTYs support Linux and macOS"
#endif

#include "Options.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
    #include <libproc.h>
    #include <sys/sysctl.h>
#else
    #include <sys/syscall.h>
#endif

namespace webfront::pty::detail {

class PtyFd {
public:
    explicit PtyFd(int descriptor = -1) : fd(descriptor) {}
    ~PtyFd() {
        reset();
    }
    PtyFd(const PtyFd&)            = delete;
    PtyFd& operator=(const PtyFd&) = delete;

    int get() const {
        return fd;
    }
    void reset(int replacement = -1) {
        if (fd >= 0)
            ::close(fd);
        fd = replacement;
    }

private:
    int fd;
};

class NativePty {
public:
    explicit NativePty(const PtyOptions& options) {
        const auto               executable = resolve(options);
        std::vector<std::string> environment;
        for (const auto& [key, value] : options.environment)
            environment.push_back(key + "=" + value);
        const auto args      = pointers(options.argv);
        const auto env       = pointers(environment);
        const auto maximumFd = descriptorLimit();
        makePipe(wakeRead, wakeWrite, true);
        auto dimensions = window(options.size);

        master.reset(::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
        if (master.get() < 0 || ::fcntl(master.get(), F_SETFL, O_NONBLOCK) < 0 || ::grantpt(master.get()) < 0 || ::unlockpt(master.get()) < 0)
            fail("open PTY master");
        std::array<char, PATH_MAX> slaveName{};
        if (const auto error = ::ptsname_r(master.get(), slaveName.data(), slaveName.size()); error)
            throw std::system_error(error > 0 ? error : errno, std::generic_category(), "resolve PTY slave");
        PtyFd slave{::open(slaveName.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
        if (slave.get() < 0 || ::ioctl(slave.get(), TIOCSWINSZ, &dimensions) < 0)
            fail("open and size PTY slave");

        // The close-on-exec pipe delivers setup errors and synchronizes with setsid/TIOCSCTTY.
        PtyFd reportRead, reportWrite;
        makePipe(reportRead, reportWrite, false);
        // Keep the error channel above stderr even when the host has closed some standard descriptors.
        const int report = ::fcntl(reportWrite.get(), F_DUPFD_CLOEXEC, 3);
        if (report < 0)
            fail("duplicate PTY launch descriptor");
        reportWrite.reset(report);
        const auto*      program   = executable.c_str();
        const auto*      directory = options.directory.c_str();
        struct sigaction childAction{};
        if (::sigaction(SIGCHLD, nullptr, &childAction) < 0)
            fail("query SIGCHLD disposition");
        if (childAction.sa_handler == SIG_IGN || (childAction.sa_flags & SA_NOCLDWAIT))
            throw std::logic_error("PTY needs the host to leave its child exit status waitable");

        // Prevent a host signal handler from running in the post-fork child.
        sigset_t blocked{}, previous{};
        (void)sigfillset(&blocked);
        if (const int error = ::pthread_sigmask(SIG_SETMASK, &blocked, &previous); error)
            throw std::system_error(error, std::generic_category(), "block signals before PTY fork");
        child = ::fork();
        if (child == 0)
            launch(program, directory, args.data(), env.data(), slave.get(), report, maximumFd);
        const int forkError = errno;
        ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (child < 0)
            throw std::system_error(forkError, std::generic_category(), "fork PTY child");
        reportWrite.reset();
        slave.reset();

        int     error = 0;
        ssize_t count;
        // Even a close-on-exec descriptor can be kept by a concurrent child that forks
        // without exec. Bound the wait even if that process holds the writer.
        pollfd     reportReady{reportRead.get(), POLLIN, 0};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            count = ::poll(&reportReady, 1, 100);
        } while ((count == 0 || (count < 0 && errno == EINTR)) && std::chrono::steady_clock::now() < deadline);
        if (count > 0) {
            do {
                count = ::read(reportRead.get(), &error, sizeof(error));
            } while (count < 0 && errno == EINTR);
        } else if (count == 0) {
            count = -1;
            errno = ETIMEDOUT;
        }
        if (count != 0) {
            if (count < 0)
                error = errno;
            ::kill(child, SIGKILL);
            finish();
            throw std::system_error(error, std::generic_category(), "start PTY child");
        }
    }

    ~NativePty() {
        finish();
    }

    // All I/O after construction belongs to the worker thread.
    std::size_t read(std::span<std::uint8_t> bytes) {
        const auto count = ::read(master.get(), bytes.data(), bytes.size());
        // EIO means no slave is open now. A live program can reopen it.
        if (count < 0 && errno != EIO && errno != EAGAIN && errno != EINTR)
            fail("read PTY output");
        if (count >= 0 || errno != EINTR)
            eof = count == 0 || (count < 0 && errno == EIO);
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }

    bool atEnd() const {
        return eof;
    }
    void beginDrain() const {}

    void wake() const {
        const char byte = 0;
        while (::write(wakeWrite.get(), &byte, 1) < 0 && errno == EINTR) {}
    }

    // The owner excludes concurrent wake() calls before retiring these descriptors.
    void closeWakePipe() {
        wakeWrite.reset();
        wakeRead.reset();
    }

    void wait(bool writing, bool draining) const {
        const short events = writing ? POLLIN | POLLOUT : POLLIN;
        // A closed slave leaves POLLHUP continuously set. Retry periodically until it reopens
        // or the leader exits. The timeout also detects exit while a descendant holds the slave.
        pollfd fds[] = {
            {         wakeRead.get(), POLLIN, 0},
            {eof ? -1 : master.get(), events, 0}
        };
        const int ready = ::poll(fds, 2, eof || draining ? 50 : 250);
        if (ready < 0 && errno != EINTR)
            fail("wait for PTY activity");
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            std::array<char, 256> bytes{};
            // Bound each drain so concurrent writers cannot starve terminal I/O.
            if (::read(wakeRead.get(), bytes.data(), bytes.size()) < 0 && errno != EINTR && errno != EAGAIN)
                fail("read PTY wake pipe");
        }
    }

    std::size_t write(std::span<const std::uint8_t> bytes) {
        const auto count = ::write(master.get(), bytes.data(), bytes.size());
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }

    void resize(PtySize size) {
        auto dimensions = window(size);
        if (::ioctl(master.get(), TIOCSWINSZ, &dimensions) < 0)
            fail("resize PTY");
    }

    bool exited() const {
        siginfo_t  info{};
        const auto result = ::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0)
            return info.si_pid != 0;
        if (errno == EINTR)
            return false;
        fail("wait for PTY child");
    }

    int finish() {
        if (child <= 0)
            return exitCode;

        // Stop if an external reaper has already collected our child and released its PID.
        siginfo_t info{};
        int       result;
        do {
            result = ::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            const auto foreground = foregroundGroup();
            const bool hasMembers = signalSession(foreground, SIGHUP);
            if (!info.si_pid || hasMembers) {
                signalSession(foreground, SIGCONT);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                // The waitable leader reserves the session ID even if the tty has forgotten its group.
                signalSession(foregroundGroup(), SIGKILL);
            }
            reap();
        }
        child = -1;
        master.reset();
        return exitCode;
    }

private:
    PtyFd master, wakeRead, wakeWrite;
    pid_t child    = -1;
    bool  eof      = false;
    int   exitCode = 255;

    [[noreturn]] static void fail(const char* action) {
        throw std::system_error(errno, std::generic_category(), action);
    }

    static winsize window(PtySize size) {
        return {size.rows, size.columns, 0, 0};
    }

    static std::vector<char*> pointers(const std::vector<std::string>& strings) {
        std::vector<char*> result;
        for (const auto& string : strings)
            result.push_back(const_cast<char*>(string.c_str()));
        result.push_back(nullptr);
        return result;
    }

    static std::filesystem::path resolve(const PtyOptions& options) {
        const auto& name = options.argv.front();
        if (name.contains('/'))
            return options.directory / name;
        const auto       entry = options.environment.find("PATH");
        std::string_view path  = entry == options.environment.end() ? std::string_view("/usr/bin:/bin") : std::string_view(entry->second);
        for (;;) {
            const auto  end       = path.find(':');
            auto        candidate = options.directory / std::filesystem::path(path.substr(0, end)) / name;
            struct stat status{};
            if (::stat(candidate.c_str(), &status) == 0 && S_ISREG(status.st_mode) && ::access(candidate.c_str(), X_OK) == 0)
                return candidate;
            if (end == std::string_view::npos)
                break;
            path.remove_prefix(end + 1);
        }
        throw std::system_error(ENOENT, std::generic_category(), "resolve PTY executable: " + name);
    }

    static void makePipe(PtyFd& readEnd, PtyFd& writeEnd, bool nonblocking) {
        std::array<int, 2> fds{};
#if defined(__linux__)
        if (::pipe2(fds.data(), O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0)) < 0)
#else
        if (::pipe(fds.data()) < 0)
#endif
            fail("create PTY pipe");
        readEnd.reset(fds[0]);
        writeEnd.reset(fds[1]);
#if !defined(__linux__)
        for (auto fd : fds)
            if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || (nonblocking && ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0))
                fail("configure PTY pipe");
#endif
    }

    static int descriptorLimit() {
#if defined(__APPLE__)
        int         maximum = 0;
        std::size_t size    = sizeof(maximum);
        if (::sysctlbyname("kern.maxfilesperproc", &maximum, &size, nullptr, 0) < 0)
            fail("query macOS descriptor limit");
        rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) < 0)
            fail("query descriptor limit");
        if ((limit.rlim_max < INT_MAX) && (static_cast<int>(limit.rlim_max) > maximum))
            maximum = static_cast<int>(limit.rlim_max);
        return maximum;
#else
        return 0;
#endif
    }

    static bool closeDescriptors(int report, [[maybe_unused]] int maximumFd) {
#if defined(__linux__)
    #if defined(SYS_close_range)
        if ((report == 3 || ::syscall(SYS_close_range, 3u, static_cast<unsigned>(report - 1), 0u) == 0)
            && ::syscall(SYS_close_range, static_cast<unsigned>(report + 1), UINT_MAX, 0u) == 0)
            return true;
    #endif
        // Use the kernel directory API directly. libc directory iteration can allocate after fork.
        const int directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            return false;
        // linux_dirent64: u64 inode, s64 offset, u16 length, u8 type, then the name.
        std::array<char, 4096> entries{};
        for (;;) {
            const long size = ::syscall(SYS_getdents64, directory, entries.data(), entries.size());
            if (size < 0 && errno == EINTR)
                continue;
            if (size <= 0) {
                const int error = errno;
                ::close(directory);
                errno = error;
                return size == 0;
            }
            for (long offset = 0; offset < size;) {
                const char*   entry  = entries.data() + offset;
                std::uint16_t length = 0;
                std::memcpy(&length, entry + 16, sizeof(length));
                int         fd   = 0;
                const auto* name = entry + 19;
                while (*name >= '0' && *name <= '9')
                    fd = fd * 10 + *name++ - '0';
                if (fd >= 3 && fd != report && fd != directory)
                    ::close(fd);
                offset += length;
            }
        }
#else
        for (int fd = 3; fd < maximumFd; ++fd)
            if (fd != report)
                ::close(fd);
        return true;
#endif
    }

    // Child branch: only system operations and preallocated data between fork and exec.
    [[noreturn]] static void launch(const char* program, const char* directory, char* const* args, char* const* env, int slave, int report, int maximumFd) {
        if (::setsid() >= 0 && ::ioctl(slave, TIOCSCTTY, 0) >= 0 && ::dup2(slave, STDIN_FILENO) >= 0 && ::dup2(slave, STDOUT_FILENO) >= 0
            && ::dup2(slave, STDERR_FILENO) >= 0 && ::chdir(directory) == 0) {
            for (int fd = 0; fd <= STDERR_FILENO; ++fd)
                ::fcntl(fd, F_SETFD, 0);

            if (closeDescriptors(report, maximumFd)) {
                struct sigaction action{};
                action.sa_handler = SIG_DFL;
                (void)sigemptyset(&action.sa_mask);
                for (int signal = 1; signal < NSIG; ++signal)
                    if (signal != SIGKILL && signal != SIGSTOP)
                        ::sigaction(signal, &action, nullptr);
                ::sigprocmask(SIG_SETMASK, &action.sa_mask, nullptr);
                ::execve(program, args, env);
            }
        }
        const auto error = errno;
        ssize_t    count;
        do {
            count = ::write(report, &error, sizeof(error));
        } while (count < 0 && errno == EINTR);
        ::_exit(127);
    }

    pid_t foregroundGroup() const {
        const auto group = ::tcgetpgrp(master.get());
#if defined(__APPLE__)
        // Darwin can require the calling process to control the tty for TIOCGPGRP.
        if (group <= 0) {
            int         request[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, child};
            kinfo_proc  process{};
            std::size_t bytes = sizeof(process);
            if (::sysctl(request, 4, &process, &bytes, nullptr, 0) == 0 && bytes == sizeof(process))
                return process.kp_eproc.e_tpgid;
        }
#endif
        return group;
    }

    bool signalSession(pid_t foreground, int signal) const {
        bool hasMembers = false;
        if (foreground > 0 && foreground != child)
            hasMembers = ::kill(-foreground, signal) == 0 || errno != ESRCH;
        ::kill(-child, signal);
        auto signalMember = [&](pid_t pid) {
            if (pid <= 0 || pid == child || ::getsid(pid) != child)
                return;
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
            PtyFd      reference{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0u))};
            const bool pidfdUnavailable = reference.get() < 0 && (errno == ENOSYS || errno == EINVAL);
#endif
            if (::getsid(pid) != child)
                return;
            hasMembers       = true;
            const auto group = ::getpgid(pid);
            if (group <= 0 || group == child || group == foreground)
                return;
#if defined(__linux__) && defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
            if (reference.get() >= 0) {
                ::syscall(SYS_pidfd_send_signal, reference.get(), signal, nullptr, 0u);
                return;
            }
            if (!pidfdUnavailable)
                return;
#endif
            // Platforms without pidfds can only recheck the process identity immediately before signalling.
            if (::getsid(pid) == child && ::getpgid(pid) == group)
                ::kill(pid, signal);
        };
#if defined(__APPLE__)
        // Listing is best-effort if the OS cannot provide a process snapshot.
        try {
            const auto needed = ::proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
            if (needed <= 0)
                return true;
            std::vector<pid_t> pids(static_cast<std::size_t>(needed) / sizeof(pid_t) + 64);
            const auto         bytes = ::proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
            if (bytes <= 0 || static_cast<std::size_t>(bytes) == pids.size() * sizeof(pid_t))
                hasMembers = true;
            for (int i = 0; i < bytes / static_cast<int>(sizeof(pid_t)); ++i)
                signalMember(pids[static_cast<std::size_t>(i)]);
        } catch (const std::bad_alloc&) {
            return true;
        }
#else
        if (auto* processes = ::opendir("/proc")) {
            while (auto* entry = ::readdir(processes)) {
                pid_t pid               = 0;
                const auto [end, error] = std::from_chars(entry->d_name, entry->d_name + std::strlen(entry->d_name), pid);
                if (error == std::errc{} && *end == 0)
                    signalMember(pid);
            }
            ::closedir(processes);
        } else {
            return true;
        }
#endif
        return hasMembers;
    }

    void reap() {
        int   status = 0;
        pid_t result;
        do {
            result = ::waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result == child)
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        child = -1;
    }
};
}  // namespace webfront::pty::detail
