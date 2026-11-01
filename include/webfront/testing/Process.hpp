/// @brief Runs one native program for a test, without a shell, with bounded time and output.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
    #include "../system/WindowsCompat.hpp"

    #include <cwchar>

    #include <windows.h>
#else
    #include <cerrno>
    #include <csignal>

    #include <fcntl.h>
    #include <poll.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char** environ;
#endif

namespace webfront::testing {

/// What to run. `program` is launched directly with `arguments` as its argv[1...]: no shell ever interprets them.
struct ProcessSpec {
    std::filesystem::path    program;
    std::vector<std::string> arguments;
    std::filesystem::path    workingDirectory;  ///< Empty: the calling process's directory.
    /// The child's whole environment, unless `inheritEnvironment` is set, in which case these entries are added to a copy
    /// of the calling process's environment (and win over it).
    std::map<std::string, std::string> environment;
    bool                               inheritEnvironment = false;
    std::chrono::milliseconds          timeout{30000};
    std::size_t                        maxOutputBytes = 1024 * 1024;  ///< Per stream; the rest is read and discarded.
};

enum class ProcessStatus {
    exited,        ///< The program ended by itself; `exitCode` is meaningful (zero or not).
    signaled,      ///< POSIX: the program was ended by a signal, in `signal`.
    timeout,       ///< The deadline passed; the program and its process group/job were killed.
    cancelled,     ///< The caller asked to stop; the program and its process group/job were killed.
    launchFailed,  ///< The program could not be started; `error` says why.
};

struct ProcessResult {
    ProcessStatus             status   = ProcessStatus::launchFailed;
    int                       exitCode = -1;
    int                       signal   = 0;
    std::string               error;
    std::string               standardOutput, standardError;
    bool                      outputTruncated = false, errorTruncated = false;
    std::chrono::milliseconds duration{0};
};

[[nodiscard]] inline const char* processStatusName(ProcessStatus status) noexcept {
    switch (status) {
        case ProcessStatus::exited:
            return "exited";
        case ProcessStatus::signaled:
            return "signaled";
        case ProcessStatus::timeout:
            return "timeout";
        case ProcessStatus::cancelled:
            return "cancelled";
        case ProcessStatus::launchFailed:
            break;
    }
    return "launchFailed";
}

namespace detail {

inline constexpr std::chrono::milliseconds processTick{20};
/// After the direct child ended, how long its inherited pipes may stay open (a descendant that left the group or job).
inline constexpr std::chrono::milliseconds processDrainGrace{1000};

inline void validateProcessSpec(const ProcessSpec& spec) {
    const auto clean = [](const std::string& text) {
        return text.find('\0') == std::string::npos;
    };
    if (spec.program.empty())
        throw std::invalid_argument("A program is required");
    if (!std::ranges::all_of(spec.arguments, clean))
        throw std::invalid_argument("An argument must not contain a NUL character");
    for (const auto& [key, value] : spec.environment)
        if (key.empty() || key.find('=') != std::string::npos || !clean(key) || !clean(value))
            throw std::invalid_argument("Invalid environment entry");
    if (spec.timeout.count() <= 0)
        throw std::invalid_argument("The timeout must be positive");
}

inline void appendBounded(std::string& into, const char* data, std::size_t size, std::size_t limit, bool& truncated) {
    const std::size_t room = into.size() < limit ? limit - into.size() : 0;
    into.append(data, std::min(size, room));
    if (size > room)
        truncated = true;
}

#ifdef _WIN32

inline std::wstring toWide(const std::string& text) {
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        throw std::invalid_argument("Text is not valid UTF-8");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

/// The CommandLineToArgvW / MSVCRT quoting: the program receives exactly the argument.
inline std::wstring quoteWindowsArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
            quoted.append(backslashes * 2 + 1, L'\\');
        else
            quoted.append(backslashes, L'\\');
        quoted.push_back(*it);
    }
    return quoted + L'"';
}

struct WindowsHandle {
    HANDLE handle   = nullptr;
    WindowsHandle() = default;
    explicit WindowsHandle(HANDLE h) : handle(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    WindowsHandle(const WindowsHandle&)            = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;
    ~WindowsHandle() {
        reset();
    }
    void reset() {
        if (handle)
            ::CloseHandle(handle);
        handle = nullptr;
    }
    explicit operator bool() const {
        return handle != nullptr;
    }
};

inline ProcessResult runProcessWindows(const ProcessSpec& spec, const std::atomic<bool>& cancel) {
    ProcessResult result;
    const auto    begin = std::chrono::steady_clock::now();
    try {
        std::wstring commandLine = quoteWindowsArgument(spec.program.wstring());
        for (const auto& argument : spec.arguments)
            commandLine += L' ' + quoteWindowsArgument(toWide(argument));
        // The environment block: sorted case-insensitively, as CreateProcess requires.
        std::map<std::wstring, std::wstring, bool (*)(const std::wstring&, const std::wstring&)> variables{[](const std::wstring& a, const std::wstring& b) {
            return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        }};
        if (spec.inheritEnvironment) {
            wchar_t* block = ::GetEnvironmentStringsW();
            for (const wchar_t* entry = block; block && *entry; entry += std::wcslen(entry) + 1) {
                const std::wstring text{entry};
                if (const auto equals = text.find(L'=', 1); equals != std::wstring::npos)
                    variables[text.substr(0, equals)] = text.substr(equals + 1);
            }
            if (block)
                ::FreeEnvironmentStringsW(block);
        } else {
            // A Windows program usually cannot start without its system directory.
            wchar_t     root[MAX_PATH];
            const DWORD length = ::GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
                variables[L"SystemRoot"] = std::wstring(root, length);
        }
        for (const auto& [key, value] : spec.environment)
            variables[toWide(key)] = toWide(value);
        std::wstring environmentBlock;
        for (const auto& [key, value] : variables)
            environmentBlock += key + L'=' + value + L'\0';
        environmentBlock += L'\0';

        SECURITY_ATTRIBUTES inheritable{sizeof inheritable, nullptr, TRUE};
        WindowsHandle       outRead, outWrite, errRead, errWrite, input;
        HANDLE              pipeEnds[2];
        if (!::CreatePipe(&pipeEnds[0], &pipeEnds[1], &inheritable, 0))
            throw std::runtime_error("Cannot create a pipe");
        outRead.handle  = pipeEnds[0];
        outWrite.handle = pipeEnds[1];
        if (!::CreatePipe(&pipeEnds[0], &pipeEnds[1], &inheritable, 0))
            throw std::runtime_error("Cannot create a pipe");
        errRead.handle  = pipeEnds[0];
        errWrite.handle = pipeEnds[1];
        ::SetHandleInformation(outRead.handle, HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(errRead.handle, HANDLE_FLAG_INHERIT, 0);
        input = WindowsHandle{::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr)};
        if (!input)
            throw std::runtime_error("Cannot open NUL");

        // Only these three handles are inherited.
        HANDLE inherited[3]  = {input.handle, outWrite.handle, errWrite.handle};
        SIZE_T attributeSize = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeStorage(attributeSize);
        auto*             attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
            throw std::runtime_error("Cannot prepare the process attributes");
        struct AttributesGuard {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~AttributesGuard() {
                ::DeleteProcThreadAttributeList(list);
            }
        } attributesGuard{attributes};
        if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr))
            throw std::runtime_error("Cannot restrict the inherited handles");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb         = sizeof startup;
        startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput  = input.handle;
        startup.StartupInfo.hStdOutput = outWrite.handle;
        startup.StartupInfo.hStdError  = errWrite.handle;
        startup.lpAttributeList        = attributes;

        WindowsHandle job{::CreateJobObjectW(nullptr, nullptr)};
        if (!job)
            throw std::runtime_error("Cannot create a job object");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &limits, sizeof limits);

        const std::wstring  directory = spec.workingDirectory.wstring();
        PROCESS_INFORMATION process{};
        if (!::CreateProcessW(spec.program.c_str(),
                              commandLine.data(),
                              nullptr,
                              nullptr,
                              TRUE,
                              CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                              environmentBlock.data(),
                              directory.empty() ? nullptr : directory.c_str(),
                              &startup.StartupInfo,
                              &process)) {
            result.error = "CreateProcess failed with error " + std::to_string(::GetLastError());
            return result;
        }
        WindowsHandle child{process.hProcess}, thread{process.hThread};
        if (!::AssignProcessToJobObject(job.handle, child.handle)) {
            ::TerminateProcess(child.handle, 1);
            result.error = "Cannot assign the program to its job object";
            return result;
        }
        ::ResumeThread(thread.handle);
        outWrite.reset();
        errWrite.reset();
        input.reset();

        std::atomic<bool> outDone{false}, errDone{false};
        const auto        reader = [](HANDLE pipe, std::string& into, bool& truncated, std::size_t limit, std::atomic<bool>& done) {
            char  buffer[8192];
            DWORD count = 0;
            while (::ReadFile(pipe, buffer, sizeof buffer, &count, nullptr) && count > 0)
                appendBounded(into, buffer, count, limit, truncated);
            done = true;
        };
        std::thread outThread{reader,
                              outRead.handle,
                              std::ref(result.standardOutput),
                              std::ref(result.outputTruncated),
                              spec.maxOutputBytes,
                              std::ref(outDone)};
        std::thread errThread{reader, errRead.handle, std::ref(result.standardError), std::ref(result.errorTruncated), spec.maxOutputBytes, std::ref(errDone)};

        const auto deadline = begin + spec.timeout;
        bool       timedOut = false, cancelled = false;
        for (;;) {
            if (::WaitForSingleObject(child.handle, static_cast<DWORD>(processTick.count())) == WAIT_OBJECT_0)
                break;
            if (cancel) {
                cancelled = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
        }
        // The program, and everything it started, ends here: its descendants do not outlive the call.
        ::TerminateJobObject(job.handle, 1);
        ::WaitForSingleObject(child.handle, 5000);
        const auto drainEnd = std::chrono::steady_clock::now() + processDrainGrace;
        while (!(outDone && errDone) && std::chrono::steady_clock::now() < drainEnd)
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        ::CancelIoEx(outRead.handle, nullptr);
        ::CancelIoEx(errRead.handle, nullptr);
        outThread.join();
        errThread.join();
        DWORD code = 0;
        ::GetExitCodeProcess(child.handle, &code);
        result.exitCode = static_cast<int>(code);
        result.status   = timedOut ? ProcessStatus::timeout : cancelled ? ProcessStatus::cancelled : ProcessStatus::exited;
        if (result.status != ProcessStatus::exited)
            result.exitCode = -1;
    } catch (const std::exception& error) {
        result.status = ProcessStatus::launchFailed;
        result.error  = error.what();
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    return result;
}

#else

struct Descriptor {
    int fd       = -1;
    Descriptor() = default;
    explicit Descriptor(int value) : fd(value) {}
    Descriptor(const Descriptor&)            = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        reset();
    }
    void reset() {
        if (fd >= 0)
            ::close(fd);
        fd = -1;
    }
};

inline bool makePipe(Descriptor& readEnd, Descriptor& writeEnd) {
    int fds[2];
    if (::pipe(fds) != 0)
        return false;
    readEnd.fd  = fds[0];
    writeEnd.fd = fds[1];
    for (const int fd : fds)
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
    ::fcntl(readEnd.fd, F_SETFL, ::fcntl(readEnd.fd, F_GETFL) | O_NONBLOCK);
    return true;
}

inline ProcessResult runProcessPosix(const ProcessSpec& spec, const std::atomic<bool>& cancel) {
    ProcessResult result;
    const auto    begin = std::chrono::steady_clock::now();

    std::vector<std::string>           environmentStrings;
    std::map<std::string, std::string> variables;
    if (spec.inheritEnvironment)
        for (char** entry = environ; entry && *entry; ++entry) {
            const std::string text{*entry};
            if (const auto equals = text.find('='); equals != std::string::npos && equals > 0)
                variables[text.substr(0, equals)] = text.substr(equals + 1);
        }
    for (const auto& [key, value] : spec.environment)
        variables[key] = value;
    for (const auto& [key, value] : variables)
        environmentStrings.push_back(key + '=' + value);
    std::vector<char*> environmentBlock;
    for (auto& entry : environmentStrings)
        environmentBlock.push_back(entry.data());
    environmentBlock.push_back(nullptr);

    const std::string        program   = spec.program.string();
    const std::string        directory = spec.workingDirectory.string();
    std::vector<std::string> argumentStrings{program};
    argumentStrings.insert(argumentStrings.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argumentStrings)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    Descriptor outRead, outWrite, errRead, errWrite;
    if (!makePipe(outRead, outWrite) || !makePipe(errRead, errWrite)) {
        result.error = "Cannot create a pipe";
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t          attributes;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawnattr_init(&attributes);
    struct Guard {
        posix_spawn_file_actions_t* actions;
        posix_spawnattr_t*          attributes;
        ~Guard() {
            ::posix_spawn_file_actions_destroy(actions);
            ::posix_spawnattr_destroy(attributes);
        }
    } guard{&actions, &attributes};
    // Its own process group: a timeout or cancellation kills the program and every descendant that stayed in it.
    sigset_t none;
    ::sigemptyset(&none);
    ::posix_spawnattr_setpgroup(&attributes, 0);
    ::posix_spawnattr_setsigmask(&attributes, &none);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    bool prepared = ::posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0
                    && ::posix_spawn_file_actions_adddup2(&actions, outWrite.fd, 1) == 0 && ::posix_spawn_file_actions_adddup2(&actions, errWrite.fd, 2) == 0;
    #if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // The calling application's own descriptors (sockets, files) stay out of the program.
    prepared = prepared && ::posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0;
    #endif
    if (!directory.empty())
        prepared = prepared && ::posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()) == 0;
    if (!prepared) {
        result.error = "Cannot prepare the child process";
        return result;
    }
    pid_t pid = 0;
    if (const int code = ::posix_spawn(&pid, program.c_str(), &actions, &attributes, argv.data(), environmentBlock.data()); code != 0) {
        result.error = std::string("Cannot start the program: ") + std::strerror(code);
        return result;
    }
    outWrite.reset();
    errWrite.reset();

    const auto killGroup = [pid] {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
    };
    const auto deadline = begin + spec.timeout;
    bool       exited = false, timedOut = false, cancelled = false;
    int        waitStatus = 0;
    auto       drainEnd   = std::chrono::steady_clock::time_point::max();
    for (;;) {
        pollfd fds[2];
        nfds_t count = 0;
        int    owners[2];
        for (auto [descriptor, id] : {
                 std::pair{&outRead, 0},
                 std::pair{&errRead, 1}
        })
            if (descriptor->fd >= 0) {
                fds[count]    = {descriptor->fd, POLLIN, 0};
                owners[count] = id;
                ++count;
            }
        if (count)
            ::poll(fds, count, static_cast<int>(processTick.count()));
        else
            std::this_thread::sleep_for(processTick);
        for (nfds_t index = 0; index < count; ++index) {
            auto& descriptor = owners[index] == 0 ? outRead : errRead;
            char  buffer[8192];
            for (;;) {
                const auto got = ::read(descriptor.fd, buffer, sizeof buffer);
                if (got > 0) {
                    owners[index] == 0
                        ? appendBounded(result.standardOutput, buffer, static_cast<std::size_t>(got), spec.maxOutputBytes, result.outputTruncated)
                        : appendBounded(result.standardError, buffer, static_cast<std::size_t>(got), spec.maxOutputBytes, result.errorTruncated);
                    continue;
                }
                if (got == 0 || (errno != EAGAIN && errno != EINTR))
                    descriptor.reset();
                break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!exited) {
            if (::waitpid(pid, &waitStatus, WNOHANG) == pid) {
                exited = true;
                killGroup();  // Whatever the program left running in its group ends with it.
                drainEnd = now + processDrainGrace;
            } else if (cancel.load() && !cancelled && !timedOut) {
                cancelled = true;
                killGroup();
            } else if (now >= deadline && !cancelled && !timedOut) {
                timedOut = true;
                killGroup();
            }
        }
        if (exited && ((outRead.fd < 0 && errRead.fd < 0) || now >= drainEnd))
            break;
    }
    if (timedOut)
        result.status = ProcessStatus::timeout;
    else if (cancelled)
        result.status = ProcessStatus::cancelled;
    else if (WIFSIGNALED(waitStatus)) {
        result.status = ProcessStatus::signaled;
        result.signal = WTERMSIG(waitStatus);
    } else {
        result.status   = ProcessStatus::exited;
        result.exitCode = WEXITSTATUS(waitStatus);
    }
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    return result;
}

#endif
}  // namespace detail

/**
 * @brief Runs `spec.program` to completion, or until `spec.timeout` or `cancel`, and returns how it ended.
 * The program is started directly (never through a shell) with its own standard input closed. Its output is captured up to
 * `maxOutputBytes` per stream and the excess is discarded (`outputTruncated`/`errorTruncated`). When it ends, times out or
 * is cancelled, the program and its descendants are killed: a POSIX process group (descendants that left it with setsid
 * survive), a Windows job object. The call blocks; run it on its own thread. Invalid input throws std::invalid_argument; a
 * program that cannot start is a `launchFailed` result.
 */
inline ProcessResult runProcess(const ProcessSpec& spec, const std::atomic<bool>& cancel) {
    detail::validateProcessSpec(spec);
#ifdef _WIN32
    return detail::runProcessWindows(spec, cancel);
#else
    return detail::runProcessPosix(spec, cancel);
#endif
}

}  // namespace webfront::testing
