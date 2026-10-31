// Optional native launcher. Implementation stays in public header-only
// components.
#include <webfront/automation/Binding.hpp>
#include <webfront/automation/Channel.hpp>
#include <webfront/automation/ExitStatus.hpp>
#include <webfront/automation/Instructions.hpp>
#include <webfront/automation/Interrupt.hpp>
#include <webfront/automation/Mcp.hpp>
#include <webfront/automation/Stdio.hpp>
#include <webfront/DeveloperApp.hpp>

#include <webfront/automation/Platform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <tlhelp32.h>
#else
#include <fcntl.h>
#endif
#ifdef __linux__
#include <cerrno>
#include <fstream>
#include <map>
#include <poll.h>
#include <ranges>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
using namespace webfront;
using namespace webfront::automation;
struct Sources {
    std::filesystem::path archive, registry, profile;
};
Sources sources;

struct Files {
    explicit Files(const std::filesystem::path&) { }
    std::optional<fs::File> open(std::filesystem::path path) const {
        const auto            name = path.relative_path().generic_string();
        std::filesystem::path file;
        if (name == "automation/environment.tgz")
            file = sources.archive;
        else if (!sources.registry.empty() && name.starts_with("automation/registry/")) {
            try {
                file = developer::projectPath(sources.registry, std::string_view(name).substr(20));
            } catch (const std::exception&) {
                return {};
            }
        } else
            return {};
        std::ifstream input(file, std::ios::binary);
        if (input && std::filesystem::is_regular_file(file))
            return fs::File{std::move(input)};
        return {};
    }
};

#ifdef _WIN32
using ProcessHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

std::uint64_t creationTime(HANDLE process) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
}

// The WebView2 browser processes this agent launched, then their descendants. A process created before its recorded
// parent belongs to an unrelated process that reused the parent's identifier.
std::vector<ProcessHandle> webViewProcesses() {
    std::vector<ProcessHandle> result;
    const HANDLE               raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE)
        return result;
    const ProcessHandle          snapshot{raw, CloseHandle};
    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W              entry{.dwSize = sizeof(PROCESSENTRY32W)};
    for (bool more = Process32FirstW(raw, &entry); more; more = Process32NextW(raw, &entry))
        entries.push_back(entry);
    struct Parent {
        DWORD         id;
        std::uint64_t created;
    };
    std::vector<Parent> parents{{GetCurrentProcessId(), creationTime(GetCurrentProcess())}};
    for (std::size_t next = 0; next < parents.size(); ++next) {
        for (const auto& candidate : entries) {
            if (candidate.th32ParentProcessID != parents[next].id)
                continue;
            if (next == 0 && CompareStringOrdinal(candidate.szExeFile, -1, L"msedgewebview2.exe", -1, TRUE) != CSTR_EQUAL)
                continue;
            ProcessHandle process{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.th32ProcessID),
                                  CloseHandle};
            if (!process)
                continue;
            const auto created = creationTime(process.get());
            if (created < parents[next].created)
                continue;
            parents.push_back({candidate.th32ProcessID, created});
            result.push_back(std::move(process));
        }
    }
    return result;
}

// WebView2 closes its browser process asynchronously after the controller is released, and that process keeps the
// profile open until it exits. Retrying the removal cannot outlast it: let it exit during the grace, then end it.
void releaseWebView(std::chrono::milliseconds grace) noexcept {
    try {
        const auto processes = webViewProcesses();
        const auto remaining = [](std::chrono::steady_clock::time_point deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            return static_cast<DWORD>(std::max<std::chrono::milliseconds::rep>(left.count(), 0));
        };
        const auto exited = std::chrono::steady_clock::now() + grace;
        for (const auto& process : processes)
            if (WaitForSingleObject(process.get(), remaining(exited)) != WAIT_OBJECT_0)
                TerminateProcess(process.get(), 1);
        const auto terminated = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        for (const auto& process : processes)
            WaitForSingleObject(process.get(), remaining(terminated));
    } catch (...) {
        // Without the process list, the removal below still retries the transient errors.
    }
}
// A separate agent process can retry after a forced exit without interpreting the profile path as shell syntax.
bool removeAfterExit(const std::filesystem::path& path) noexcept {
    try {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size())
            return false;
        executable.resize(length);
        const std::wstring original = L"\"" + executable + L"\" --cleanup-profile \"" + path.wstring() + L"\"";
        STARTUPINFOW        startup{};
        startup.cb = sizeof startup;
        PROCESS_INFORMATION information{};
        for (const DWORD breakaway : {DWORD{CREATE_BREAKAWAY_FROM_JOB}, DWORD{0}}) {
            auto command = original;  // CreateProcessW may modify the command line even on failure.
            if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | breakaway, nullptr, nullptr,
                               &startup, &information)) {
                CloseHandle(information.hThread);
                CloseHandle(information.hProcess);
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

int cleanupProfile() {
    // The launcher supplies a quoted path as the final argument. Read the Unicode command line directly: argv uses the
    // process code page and cannot represent every Windows path.
    const std::wstring command = GetCommandLineW();
    const std::wstring marker = L" --cleanup-profile \"";
    const auto begin = command.find(marker);
    if (begin == std::wstring::npos || command.back() != L'"')
        return 1;
    const std::filesystem::path path{command.substr(begin + marker.size(), command.size() - begin - marker.size() - 1)};
    for (int attempt = 0; attempt != 20; ++attempt) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (!error)
            return 0;
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    return 1;
}
#elif defined(__linux__)
// The parent and state ('Z' for a zombie) of a process, from /proc/<pid>/stat; nothing when it is gone.
std::optional<std::pair<pid_t, char>> processStat(pid_t pid) {
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string   stat;
    if (!std::getline(input, stat))
        return std::nullopt;
    // The command name may contain spaces and parentheses: the fields resume after its last ')'.
    const auto end = stat.rfind(") ");
    if (end == std::string::npos || end + 4 >= stat.size())
        return std::nullopt;
    std::istringstream fields(stat.substr(end + 2));
    char               state{};
    pid_t              parent{};
    if (!(fields >> state >> parent))
        return std::nullopt;
    return std::pair{parent, state};
}

// The command name of a process, from /proc/<process>/comm, which stays readable when its executable link is not.
std::string commandName(const std::string& process) {
    std::ifstream input("/proc/" + process + "/comm");
    std::string   name;
    std::getline(input, name);
    return name;
}

// Whether a process runs the agent's executable. Chromium makes its sandboxed helpers non-dumpable, which hides their
// executable link from a process without CAP_SYS_PTRACE: such a helper is recognized by its command name, which
// Chromium does not rename for a process's main thread.
bool runsAgent(pid_t pid, const std::string& agentName) {
    const auto      process = std::to_string(pid);
    std::error_code error;
    if (std::filesystem::equivalent("/proc/" + process + "/exe", "/proc/self/exe", error))
        return true;
    return error == std::errc::permission_denied && !agentName.empty() && commandName(process) == agentName;
}

// The CEF helpers (zygote, renderers, GPU and network services) are the descendants running the agent's own
// executable, listed before any is ended (an orphan is reparented and leaves the tree): a WebKitGTK web process or a
// command the developer application started is not one of them.
std::vector<pid_t> browserHelpers() {
    std::map<pid_t, std::vector<pid_t>> children;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        const auto name = entry.path().filename().string();
        if (name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        const auto pid = static_cast<pid_t>(std::stol(name));
        if (const auto stat = processStat(pid))
            children[stat->first].push_back(pid);
    }
    std::vector<pid_t> tree{::getpid()};
    for (std::size_t next = 0; next != tree.size(); ++next)
        if (const auto found = children.find(tree[next]); found != children.end())
            tree.insert(tree.end(), found->second.begin(), found->second.end());
    const auto         agentName = commandName("self");
    std::vector<pid_t> result;
    for (const auto pid : tree | std::views::drop(1))
        if (runsAgent(pid, agentName))
            result.push_back(pid);
    return result;
}

// A pidfd keeps designating the process it was opened on, even once its identifier is reused; -1 with errno ESRCH for
// a process that is gone, ENOSYS without pidfds.
int openProcess([[maybe_unused]] pid_t pid) noexcept {
#ifdef SYS_pidfd_open
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
    errno = ENOSYS;
    return -1;
#endif
}

struct Helper {
    pid_t pid;
    int   descriptor;
    bool  killed = false;
    bool  gone   = false;  // without a pidfd, its identifier no longer designates one of the helpers
    bool  exited() const {
        if (gone)
            return true;
        if (descriptor < 0) {
            const auto stat = processStat(pid);
            return !stat || stat->second == 'Z' || stat->second == 'X';
        }
        pollfd readable{.fd = descriptor, .events = POLLIN, .revents = 0};
        return ::poll(&readable, 1, 0) > 0;
    }
    void kill() {
        if (std::exchange(killed, true))
            return;
#ifdef SYS_pidfd_send_signal
        if (descriptor >= 0) {
            ::syscall(SYS_pidfd_send_signal, descriptor, SIGKILL, nullptr, 0);
            return;
        }
#endif
        ::kill(pid, SIGKILL);
    }
};

// The CEF helpers outlive _Exit: one still writing into the profile after its removal recreates it. Let them exit
// during the grace, then end them and wait until they are gone, for 2 s more at most. They are listed once: the
// browser relaunches an ended GPU process, and aborts after a few, so a helper started later is left to the removal
// after the agent's exit.
void releaseBrowserProcesses(std::chrono::milliseconds grace) noexcept {
    std::vector<Helper> helpers;
    try {
        const auto listed = browserHelpers();
        helpers.reserve(listed.size());
        for (const auto pid : listed) {
            // A helper gone since the scan needs nothing more, and its identifier may already designate another
            // process. Without a pidfd (a kernel before Linux 5.3, a seccomp filter, no descriptor left), the helper is
            // still followed by its identifier, which is checked before it is signalled.
            const int descriptor = openProcess(pid);
            if (descriptor >= 0 || errno != ESRCH)
                helpers.push_back({pid, descriptor});
        }
        // Opened after the scan: a pidfd designates one of our helpers only if its identifier still is one.
        const auto confirmed = browserHelpers();
        std::erase_if(helpers, [&](const Helper& helper) {
            if (helper.descriptor < 0 || std::ranges::find(confirmed, helper.pid) != confirmed.end())
                return false;
            ::close(helper.descriptor);
            return true;
        });
        const auto ended      = std::chrono::steady_clock::now() + grace;
        const auto terminated = ended + std::chrono::seconds{2};
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (std::ranges::all_of(helpers, &Helper::exited) || now >= terminated)
                break;
            if (now >= ended) {
                // An identifier without a pidfd is signalled only if a scan just before, while none is ended yet,
                // still finds it among the helpers: otherwise it may designate another process.
                const auto unverified = [](const Helper& helper) {
                    return helper.descriptor < 0 && !helper.killed && !helper.exited();
                };
                const auto current = std::ranges::any_of(helpers, unverified) ? browserHelpers() : std::vector<pid_t>{};
                for (auto& helper : helpers) {
                    if (unverified(helper) && std::ranges::find(current, helper.pid) == current.end())
                        helper.gone = true;
                    if (!helper.exited())
                        helper.kill();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    } catch (...) {
        // Without the process list, the removal below still runs.
    }
    for (const auto& helper : helpers)
        if (helper.descriptor >= 0)
            ::close(helper.descriptor);
}

// Closes every descriptor from 3 in the spawned process. glibc 2.34 has a dedicated action; elsewhere the open
// descriptors are listed from /proc/self/fd, after the listing has closed its own. That list is a snapshot: a
// descriptor another thread opens without O_CLOEXEC before the spawn still reaches the shell. Chromium opens its
// descriptors with O_CLOEXEC, and the shell holds them for its bounded wait at most.
bool closeInheritedDescriptors(posix_spawn_file_actions_t& actions) {
// Another libc has no __GLIBC_PREREQ, which the preprocessor would still have to parse.
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    return ::posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0;
#else
    std::vector<int> open;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        open.push_back(std::stoi(entry.path().filename().string()));
    return std::ranges::all_of(open, [&](int descriptor) {
        return descriptor < 3 || ::fcntl(descriptor, F_GETFD) < 0 || ::posix_spawn_file_actions_addclose(&actions, descriptor) == 0;
    });
#endif
}

// The CEF browser runs inside the agent: until the agent exits, its threads may write into the profile again, and
// relaunch the helpers ended above. A shell outside the agent's descriptors and process group removes the profile once
// the agent is gone (exited, even if not reaped yet); false when it cannot be started.
bool removeAfterExit(const std::filesystem::path& path) noexcept {
    try {
        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0)
            return false;
        const auto destroyActions = [](posix_spawn_file_actions_t* initialized) {
            ::posix_spawn_file_actions_destroy(initialized);
        };
        const std::unique_ptr<posix_spawn_file_actions_t, decltype(destroyActions)> actionsGuard{&actions, destroyActions};
        posix_spawnattr_t attributes;
        if (::posix_spawnattr_init(&attributes) != 0)
            return false;
        const auto destroyAttributes = [](posix_spawnattr_t* initialized) {
            ::posix_spawnattr_destroy(initialized);
        };
        const std::unique_ptr<posix_spawnattr_t, decltype(destroyAttributes)> attributesGuard{&attributes, destroyAttributes};
#ifdef POSIX_SPAWN_SETSID
        // A kill of the agent's process group, by a harness or a CTest timeout, must not prevent the removal.
        if (::posix_spawnattr_setflags(&attributes, static_cast<short>(POSIX_SPAWN_SETSID)) != 0)
            return false;
#endif
        // Neither the CLI/MCP pipes, whose EOF the clients wait for, nor the agent's sockets go to the shell.
        bool       prepared = ::posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0
                     && ::posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0) == 0
                     && ::posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0) == 0
                     && closeInheritedDescriptors(actions);
        const auto agent   = std::to_string(::getpid());
        const auto profile = path.string();
        // Waits while the agent runs, for about a minute at most: its stat fields after the command name start with the
        // state ('Z' once exited) and have its start time 20th, which tells a reused PID apart. They are read with
        // builtins, so a poll only starts sleep. A helper relaunched just before the agent's exit ends once the browser
        // is gone, but may recreate the profile meanwhile: the removal is repeated for 2 s.
        const char* script = "probe() { read -r line 2>/dev/null < \"/proc/$0/stat\" || return 1; "
                             "set -- ${line##*\") \"}; state=$1; start=${20}; }; "
                             "probe && started=$start; i=0; "
                             "while [ $i -lt 1200 ] && probe && [ \"$state\" != Z ] && [ \"$start\" = \"$started\" ]; "
                             "do sleep 0.05; i=$((i + 1)); done; "
                             "rm -rf -- \"$1\"; i=0; while [ $i -lt 20 ]; do sleep 0.1; "
                             "if [ -e \"$1\" ]; then rm -rf -- \"$1\"; fi; i=$((i + 1)); done";
        char* const arguments[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(script),
                                   const_cast<char*>(agent.c_str()), const_cast<char*>(profile.c_str()), nullptr};
        pid_t cleaner{};
        return prepared && ::posix_spawn(&cleaner, "/bin/sh", &actions, &attributes, arguments, environ) == 0;
    } catch (...) {
        return false;
    }
}
#endif

struct Profile {
    channel::PrivateDirectory   directory{std::filesystem::temp_directory_path() / ("webfront-agent-" + channel::nonce())};
    const std::filesystem::path path{directory.path};
#ifdef __linux__
    mutable std::atomic<bool> released{false};
#endif
    /// @param grace how long the browser processes may take to exit on their own before they are ended (Windows and
    /// Linux)
    /// @param browserRunning whether the browser may still write into the profile until the agent exits (a forced exit;
    /// Linux only)
    void cleanup([[maybe_unused]] std::chrono::milliseconds grace, [[maybe_unused]] bool browserRunning = false) const noexcept {
#ifdef __linux__
        // A forced exit that loses the race with a normal one cleans up twice: the second would end the first's shell.
        if (released.exchange(true))
            return;
        releaseBrowserProcesses(grace);  // the browser's helpers would otherwise recreate the profile
        std::error_code error;
        std::filesystem::remove_all(path, error);
        // After a normal exit, CefShutdown has returned: a removed profile stays removed. Otherwise, what the browser
        // still writes goes after the agent's exit.
        if (!browserRunning && !error && !std::filesystem::exists(path, error) && !error)
            return;
        if (!removeAfterExit(path))
            std::cerr << "Automation: cannot schedule the removal of the temporary profile\n";
#else
        // POSIX can unlink open files. On Windows, the files of a process that has just exited can stay pending
        // deletion for a moment, which reports a sharing violation, a refused access or a non-empty directory.
#ifdef _WIN32
        releaseWebView(grace);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
#endif
        for (;;) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (!error)
                return;
#ifdef _WIN32
            if ((error.value() == ERROR_SHARING_VIOLATION || error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_DIR_NOT_EMPTY)
                && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            if (removeAfterExit(path))
                return;
#endif
            std::cerr << "Automation: cannot remove temporary profile: " << error.message() << '\n';
            return;
        }
#endif
    }
    ~Profile() {
        cleanup(std::chrono::seconds{5});
    }
};

struct WebViewOptions {
    static frontend::WebView2Options get() {
        frontend::WebView2Options options;
        options.userDataFolder = sources.profile;
        return options;
    }
};

struct CEFAgentFrontend : frontend::CEFFrontend {
    static void initialize() {
        webfront::detail::ensureFrontendInitialized<frontend::CEFFrontend>();
    }
    static void open(std::string_view port, std::string_view page) {
        open("127.0.0.1", port, page, {});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy) {
        open(address, port, page, policy, WindowOptions{});
    }
    static void open(std::string_view address, std::string_view port, std::string_view page, const ContentPolicy& policy, const WindowOptions& window) {
#if defined(WEBFRONT_EMBED_CEF) && defined(__linux__)
        webfront::cef::detail::openWindow(frontend::detail::applicationUrl(address, port, page),
                                          webfront::detail::BoundContentPolicy{webfront::detail::applicationOrigin(address, port), policy},
                                          nullptr,
                                          sources.profile, window, true);
#else
        frontend::CEFFrontend::open(address, port, page, policy, window);
#endif
    }
};

static_assert(frontend::FrontendType<CEFAgentFrontend>);
using Selected = WEBFRONT_AUTOMATION_FRONTEND_TYPE;
using Engine   = std::conditional_t<
    std::is_same_v<Selected, frontend::CEFFrontend>,
    CEFAgentFrontend,
    std::conditional_t<std::is_same_v<Selected, frontend::WebView2Frontend>, frontend::WebView2FrontendWithOptions<WebViewOptions>, Selected>>;

struct Options {
    SessionOptions                     session;
    std::string                        integrity, manifestDirectory, operation;
    std::filesystem::path              listen;
    std::map<std::string, std::string> arguments;
    bool                               serve = false;
    std::chrono::seconds               startupTimeout{120};
};
std::string argumentName(std::string_view flag) {
    const auto& fields = argumentProperties();
    for (const auto& [key, schema] : fields.members()) {
        std::string option = "--";
        for (char c : key) {
            if (c >= 'A' && c <= 'Z') {
                option += '-';
                option += static_cast<char>(c - 'A' + 'a');
            } else
                option += c;
        }
        if (option == flag)
            return key;
    }
    return {};
}

int timeoutSeconds(std::string_view text) {
    int        seconds      = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds < 1 || seconds > 3600)
        throw std::invalid_argument("Timeout must be 1..3600 whole seconds");
    return seconds;
}

template <typename Backend>
int runCli(Backend&                                   controller,
           std::string_view                           operation,
           const std::map<std::string, std::string>& arguments,
           std::chrono::seconds                       startupTimeout,
           std::stop_token                            stop = {}) {
    const auto  deadline = std::chrono::steady_clock::now() + startupTimeout;
    auto        delay    = std::chrono::milliseconds{100};
    std::string identity;
    while (!stop.stop_requested()) {
        if (std::chrono::steady_clock::now() > deadline)
            throw OperationError("timeout", "Developer startup timed out");
        const Object snapshot{[&] {
            if constexpr (requires { controller.describe(deadline); })
                return controller.describe(deadline);
            else
                return controller.describe();
        }()};
        if (std::chrono::steady_clock::now() >= deadline)
            throw OperationError("timeout", "Developer startup timed out");
        if (snapshot.string("state") == "closed")
            throw OperationError("disconnected", "Developer session is closed");
        if (snapshot.string("state") != "starting") {
            identity = snapshot.string("session");
            break;
        }
        std::this_thread::sleep_until(std::min(deadline, std::chrono::steady_clock::now() + delay));
        delay = std::min(delay * 2, std::chrono::milliseconds{1000});
    }
    if (stop.stop_requested())
        return 1;
    const Object request{
        objectJson({{"session", json::quote(identity)}, {"operation", json::quote(operation)}, {"arguments", objectJson(arguments)}}
        )
    };
    const auto   id = Object(controller.start(request)).string("runId");
    const Object query{
        objectJson({{"session", json::quote(identity)}, {"runId", json::quote(id)}}
        )
    };
    delay = std::chrono::milliseconds{50};
    while (!stop.stop_requested()) {
        const auto result = controller.result(query);
        const auto state  = Object(result, 256000).string("state");
        if (state != "queued" && state != "running") {
            std::cout << result << '\n' << std::flush;
            return state == "succeeded" ? 0 : 1;
        }
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds{1000});
    }
    return 1;
}

template <typename Backend, typename Closed>
void runMcp(Backend& controller, std::stop_token stop, Closed&& closed) {
    Mcp         mcp{controller};
    StdioInput  input;
    std::string line;
    while (!stop.stop_requested() && !closed()) {
        const auto read = input.read(line);
        if (read == StdioInput::Status::closed)
            break;
        if (read == StdioInput::Status::line) {
            if (const auto reply = mcp.receive(line))
                std::cout << *reply << '\n' << std::flush;
            if (!std::cout)
                break;
        }
    }
}

int attach(int argc, char** argv) {
    if (argc < 3)
        throw std::invalid_argument("--attach needs the explicitly authorized endpoint directory");
    std::string                        operation;
    std::map<std::string, std::string> arguments;
    bool                               shutdown = false;
    std::chrono::seconds               startupTimeout{120}, requestTimeout{30};
    for (int index = 3; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--mcp")
            continue;
        if (flag == "--shutdown") {
            shutdown = true;
            continue;
        }
        if (++index == argc)
            throw std::invalid_argument("Attachment argument needs a value");
        if (flag == "--operation")
            operation = argv[index];
        else if (flag == "--startup-timeout")
            startupTimeout = std::chrono::seconds{timeoutSeconds(argv[index])};
        else if (flag == "--request-timeout")
            requestTimeout = std::chrono::seconds{timeoutSeconds(argv[index])};
        else if (const auto name = argumentName(flag); !name.empty())
            arguments[name] = json::quote(argv[index]);
        else
            throw std::invalid_argument("Unknown attachment argument");
    }
    ChannelClient controller{argv[2], requestTimeout};
    if (shutdown) {
        if (!operation.empty())
            throw std::invalid_argument("--shutdown cannot execute an operation");
        controller.shutdown();
        return 0;
    }
    if (!operation.empty())
        return runCli(controller, operation, arguments, startupTimeout);
    runMcp(controller, {}, [] {
        return false;
    });
    return 0;
}

Options parse(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto        value    = [&]() -> std::string {
            if (++i == argc)
                throw std::invalid_argument(argument + " needs a value");
            return argv[i];
        };
        if (argument == "--project")
            result.session.project = value();
        else if (argument == "--environment")
            sources.archive = value();
        else if (argument == "--integrity")
            result.integrity = value();
        else if (argument == "--registry-dir")
            sources.registry = value();
        else if (argument == "--manifest-dir")
            result.manifestDirectory = value();
        else if (argument == "--allow")
            result.session.allow.push_back(value());
        else if (argument == "--operation")
            result.operation = value();
        else if (argument == "--listen")
            result.listen = value();
        else if (argument == "--serve")
            result.serve = true;
        else if (const auto field = argumentName(argument); !field.empty())
            result.arguments[field] = json::quote(value());
        else if (argument == "--mcp") {
        } else if (argument == "--timeout" || argument == "--startup-timeout") {
            const int seconds = timeoutSeconds(value());
            if (argument == "--startup-timeout")
                result.startupTimeout = std::chrono::seconds{seconds};
            else
                result.session.timeout = std::chrono::seconds{seconds};
        } else
            throw std::invalid_argument("Unknown argument: " + argument);
    }
    if (result.session.project.empty() || sources.archive.empty() || !result.integrity.starts_with("sha512-"))
        throw std::invalid_argument("Use --project <directory> --environment <archive> --integrity "
                                    "<sha512-SRI> [--allow <operation>] [--mcp | --operation <name>]");
    sources.archive = std::filesystem::canonical(sources.archive);
    if (!std::filesystem::is_regular_file(sources.archive))
        throw std::invalid_argument("Environment must be a file");
    if (!sources.registry.empty())
        sources.registry = developer::canonicalDirectory(sources.registry);
    if (result.serve && (result.listen.empty() || !result.operation.empty()))
        throw std::invalid_argument("--serve requires --listen and cannot execute a CLI operation");
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        if (argc == 3 && std::string_view(argv[1]) == "--cleanup-profile")
            return cleanupProfile();
#endif
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif
        if (argc == 2 && std::string_view(argv[1]) == "--schema") {
            std::cout << contractSource << '\n';
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--agent-help") {
            std::cout << agentInstructions;
            return 0;
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--attach")
            return attach(argc, argv);
#ifndef _WIN32
        // A renderer surviving forced termination must not retain the agent's
        // CLI/MCP pipes: clients otherwise wait for EOF after the agent exits.
        for (int descriptor = 0; descriptor != 3; ++descriptor) {
            const auto flags = ::fcntl(descriptor, F_GETFD);
            if (flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
                throw std::runtime_error("Cannot isolate automation stdio from renderer subprocesses");
        }
#endif
        // Dispatch CEF renderer processes before parsing flags or creating a
        // profile.
        webfront::detail::ensureFrontendInitialized<Selected>();
        const auto options = parse(argc, argv);
        Interrupt interrupt;
        Profile    profile;
        sources.profile = profile.path;
        auto session    = std::make_shared<Session>(options.session);
        using App       = BasicDeveloperApp<Engine, Files>;
        App app{options.session.project, {.port = "0"}};
        app.bootstrap("developer",
                      {.environment       = "/automation/environment.tgz",
                       .integrity         = options.integrity,
                       .registry          = sources.registry.empty() ? std::nullopt : std::optional<std::string>{"/automation/registry/"},
                       .manifestDirectory = options.manifestDirectory});
        Binding                      binding{app, session};
        std::optional<ChannelServer> channel;
        if (!options.listen.empty())
            channel.emplace(session, options.listen);
        std::atomic<int>  status{0};
        std::atomic<bool> done{false};
        std::atomic<bool> finished{false};  // the transport completed its work: a closed session is then a normal ending
        const auto        launched = std::chrono::steady_clock::now();
        std::jthread      supervisor([&](std::stop_token stop) {
            while (!stop.stop_requested() && !done.load()) {
                if (const auto number = interrupt.signal()) {
                    status.store(128 + number);
                    session->close("Interrupted by native signal");
                }
                const bool starting = !session->wasReady() && !session->isClosed();
                if (session->expire() || (starting && std::chrono::steady_clock::now() - launched > options.startupTimeout)) {
                    status.store(71);
                    session->close("Native automation deadline exceeded");
                }
                if (session->isClosed()) {
                    binding.closeBrowser();
                    // Neither a blocked renderer nor a blocked stdio writer may keep
                    // shutdown alive forever.
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                    while (!stop.stop_requested() && !done.load() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds{20});
                    if (!stop.stop_requested() && !done.load()) {
                        // A normal ending keeps its own status; only an abnormal one is reported as stuck.
                        const int code = status.load() ? status.load() : finished.load() ? 0 : 72;
                        if (channel)
                            channel->stop();
                        // _Exit skips the destructors. The browser already had the close grace above: end it now.
                        profile.cleanup(std::chrono::milliseconds{0}, true);
                        // Ending the browser can release a blocked main thread: an exit completed meanwhile keeps its
                        // own status and destructors.
                        if (!done.load())
                            std::_Exit(code);
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
        });
        std::jthread      transport([&](std::stop_token stop) {
            try {
                if (options.serve) {
                    while (!stop.stop_requested() && !session->isClosed())
                        std::this_thread::sleep_for(std::chrono::milliseconds{20});
                } else if (!options.operation.empty()) {
                    const auto result = runCli(*session, options.operation, options.arguments, options.startupTimeout, stop);
                    int        expected = 0;
                    status.compare_exchange_strong(expected, result);
                } else
                    runMcp(*session, stop, [&] {
                        return session->isClosed();
                    });
                finished.store(true);
            } catch (const std::exception& error) {
                std::cerr << "Automation: " << error.what() << '\n';
                int expected = 0;
                status.compare_exchange_strong(expected, 1);
            }
            session->close();
        });
        app.show(binding.launchPage());
        if (const auto number = interrupt.signal())
            status.store(128 + number);
        if (!session->wasReady()) {
            const auto failure = Object(session->describe()).string("failure");
            if (!failure.empty()) {
                std::cerr << "Automation startup: " << failure << '\n';
                recordBootstrapFailure(status);
            }
        }
        session->close("Developer window closed");
        transport.request_stop();
        app.shutdown();
        transport.join();
        done.store(true);
        supervisor.request_stop();
        return status.load();
    } catch (const TransportError& error) {
        std::cerr << "webfront-agent: " << error.what() << "; request outcome may be unknown\n";
        return 69;
    } catch (const OperationError& error) {
        std::cerr << "webfront-agent: " << error.what() << '\n';
        return error.code == "timeout" ? 71 : error.code == "disconnected" ? 69 : 1;
    } catch (const std::exception& error) {
        std::cerr << "webfront-agent: " << error.what() << '\n';
        return 64;
    }
}
