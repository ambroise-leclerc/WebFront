#pragma once

#include "Options.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
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
        const auto args       = pointers(options.argv);
        const auto env        = pointers(environment);
        const auto maximumFd  = descriptorLimit();
        auto       dimensions = window(options.size);

        master.reset(::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
        if (master.get() < 0 || ::grantpt(master.get()) < 0 || ::unlockpt(master.get()) < 0)
            fail("open PTY master");
        std::array<char, PATH_MAX> slaveName{};
        if (const auto error = ::ptsname_r(master.get(), slaveName.data(), slaveName.size()); error)
            throw std::system_error(error > 0 ? error : errno, std::generic_category(), "resolve PTY slave");
        PtyFd slave{::open(slaveName.data(), O_RDWR | O_NOCTTY | O_CLOEXEC)};
        if (slave.get() < 0 || ::ioctl(slave.get(), TIOCSWINSZ, &dimensions) < 0)
            fail("open and size PTY slave");

        // The close-on-exec pipe delivers setup errors and synchronizes with setsid/TIOCSCTTY.
        std::array<int, 2> fds{};
#if defined(__linux__)
        if (::pipe2(fds.data(), O_CLOEXEC) < 0)
#else
        if (::pipe(fds.data()) < 0)
#endif
            fail("create PTY launch pipe");
        PtyFd reportRead{fds[0]}, reportWrite{fds[1]};
#if !defined(__linux__)
        if (::fcntl(reportRead.get(), F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(reportWrite.get(), F_SETFD, FD_CLOEXEC) < 0)
            fail("protect PTY launch descriptors");
#endif
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
        do {
            count = ::read(reportRead.get(), &error, sizeof(error));
        } while (count < 0 && errno == EINTR);
        if (count != 0) {
            if (count < 0)
                error = errno;
            ::kill(child, SIGKILL);
            reap();
            throw std::system_error(error, std::generic_category(), "start PTY child");
        }
    }

    ~NativePty() {
        finish();
    }

    // All I/O after construction belongs to the worker thread.
    std::size_t read(std::span<std::uint8_t> bytes) {
        if (eof)
            return 0;
        const auto count = ::read(master.get(), bytes.data(), bytes.size());
        if (count > 0)
            return static_cast<std::size_t>(count);
        if (count == 0 || (errno != EAGAIN && errno != EINTR))
            eof = true;
        return 0;
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
            // A job-control shell can put its foreground job in a different process group.
            const auto foreground = foregroundGroup();
            signalGroups(foreground, SIGHUP);
            signalGroups(foreground, SIGCONT);
            if (!info.si_pid || (foreground > 0 && foreground != child))
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // The leader remains waitable, reserving its PID during group cleanup.
            const auto currentForeground = foregroundGroup();
            if (currentForeground > 0 && currentForeground != foreground && currentForeground != child)
                ::kill(-currentForeground, SIGKILL);
            signalGroups(foreground, SIGKILL);
            reap();
        }
        child = -1;
        master.reset();
        return exitCode;
    }

private:
    PtyFd master;
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

    static int descriptorLimit() {
        rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) < 0)
            fail("query descriptor limit");
#if defined(__APPLE__)
        int         maximum = 0;
        std::size_t size    = sizeof(maximum);
        if (::sysctlbyname("kern.maxfilesperproc", &maximum, &size, nullptr, 0) < 0)
            fail("query macOS descriptor limit");
        if (limit.rlim_max<INT_MAX&& static_cast<int>(limit.rlim_max)> maximum)
            maximum = static_cast<int>(limit.rlim_max);
        return maximum;
#else
        return limit.rlim_max < INT_MAX ? static_cast<int>(limit.rlim_max) : INT_MAX;
#endif
    }

    // Child branch: only system operations and preallocated data between fork and exec.
    [[noreturn]] static void launch(const char* program, const char* directory, char* const* args, char* const* env, int slave, int report, int maximumFd) {
        if (::setsid() >= 0 && ::ioctl(slave, TIOCSCTTY, 0) >= 0 && ::dup2(slave, STDIN_FILENO) >= 0 && ::dup2(slave, STDOUT_FILENO) >= 0
            && ::dup2(slave, STDERR_FILENO) >= 0 && ::chdir(directory) == 0) {
            for (int fd = 0; fd <= STDERR_FILENO; ++fd)
                ::fcntl(fd, F_SETFD, 0);

            bool closed = false;
#if defined(SYS_close_range)
            closed = (report == 3 || ::syscall(SYS_close_range, 3u, static_cast<unsigned>(report - 1), 0u) == 0)
                     && ::syscall(SYS_close_range, static_cast<unsigned>(report + 1), UINT_MAX, 0u) == 0;
#endif
            if (!closed)
                for (int fd = 3; fd < maximumFd; ++fd)
                    if (fd != report)
                        ::close(fd);

            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            (void)sigemptyset(&action.sa_mask);
            for (int signal = 1; signal < NSIG; ++signal)
                if (signal != SIGKILL && signal != SIGSTOP)
                    ::sigaction(signal, &action, nullptr);
            ::sigprocmask(SIG_SETMASK, &action.sa_mask, nullptr);
            ::execve(program, args, env);
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

    void signalGroups(pid_t foreground, int signal) const {
        if (foreground > 0 && foreground != child)
            ::kill(-foreground, signal);
        ::kill(-child, signal);
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
