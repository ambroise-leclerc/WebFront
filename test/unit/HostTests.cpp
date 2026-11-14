#include "Bdd.hpp"
#include <webfront/testing/Host.hpp>
#include <webfront/testing/Process.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace webfront::bdd;
#ifndef _WIN32
    #include <csignal>

    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace {
using namespace webfront::testing;
using namespace std::chrono_literals;

const std::filesystem::path probe{WEBFRONT_HOST_PROBE};

ProcessSpec probeSpec(std::vector<std::string> arguments) {
    ProcessSpec spec;
    spec.program   = probe;
    spec.arguments = std::move(arguments);
    return spec;
}

ProcessResult run(const ProcessSpec& spec) {
    const std::atomic<bool> never{false};
    return runProcess(spec, never);
}

struct TempRoot {
    std::filesystem::path path;
    explicit TempRoot(const std::string& name) : path(std::filesystem::temp_directory_path() / ("webfront-host-" + name)) {
        std::filesystem::remove_all(path);
    }
    ~TempRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

std::string configurationFor(const TempRoot& root, const std::vector<std::filesystem::path>& programs) {
    std::string text = "{\"programs\":[";
    for (const auto& program : programs)
        text += (text.size() > 13 ? "," : "") + webfront::json::quote(program.generic_string());
    return text + "],\"temporary\":" + webfront::json::quote(root.path.generic_string()) + "}";
}

std::string field(const std::string& json, const std::string& name) {
    const auto key = "\"" + name + "\":";
    auto       at  = json.find(key);
    require(at != std::string::npos);
    at              += key.size();
    const auto end   = json.find_first_of(",}", at);
    auto       value = json.substr(at, end - at);
    if (value.size() >= 2 && value.front() == '"')
        value = value.substr(1, value.size() - 2);
    return value;
}

std::string waitFor(HostSession& host, double id) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (const auto summary = host.runPoll(id))
            return *summary;
        std::this_thread::sleep_for(10ms);
    }
    failCheck("the run did not end");
    return {};
}

std::string text(const std::vector<std::uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}
}  // namespace

Scenario programReceives{"A program receives its arguments literally, without a shell", [] {
    given("arguments with spaces, quotes, Unicode, shell metacharacters and an empty one", [&] {
        const std::vector<std::string>
             arguments{"two words", "", "a;b|c&d $HOME `id` $(id) *", "\"quoted\"", "back\\slash\\", "caf\xC3\xA9 \xE2\x82\xAC", "--flag=x y"};
        auto spec = probeSpec({"args"});
        spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
        const auto result = run(spec);
        then("each one arrives as one exact argument", [&] {
            require(result.status == ProcessStatus::exited);
            require(result.exitCode == 0);
            std::string expected;
            for (std::size_t index = 0; index < arguments.size(); ++index)
                expected += "[" + std::to_string(index) + "]" + arguments[index] + "\n";
            require(result.standardOutput == expected);
        });
    });
}};

Scenario everyWay{"Every way a program can end has a structured result", [] {
    when("it exits with a non-zero code", [&] {
        const auto result = run(probeSpec({"exit", "7"}));
        then("the code is reported", [&] {
            require(result.status == ProcessStatus::exited);
            require(result.exitCode == 7);
        });
    });
    when("it writes to both streams", [&] {
        const auto result = run(probeSpec({"streams"}));
        then("each stream is captured separately", [&] {
            require(result.standardOutput == "to-stdout");
            require(result.standardError == "to-stderr");
            require(result.exitCode == 3);
        });
    });
    when("the program does not exist", [&] {
        ProcessSpec spec;
        spec.program      = probe.parent_path() / "no-such-program";
        const auto result = run(spec);
        then("the launch failure is reported with a reason, not as an exit code", [&] {
            require(result.status == ProcessStatus::launchFailed);
            require(!result.error.empty());
        });
    });
    when("it outlives its timeout", [&] {
        auto spec         = probeSpec({"hang"});
        spec.timeout      = 300ms;
        const auto result = run(spec);
        then("it is killed and reported as a timeout", [&] {
            require(result.status == ProcessStatus::timeout);
            require(result.duration < 5s);
        });
    });
    when("the caller cancels it", [&] {
        std::atomic<bool> cancel{false};
        std::thread       canceller{[&] {
            std::this_thread::sleep_for(200ms);
            cancel = true;
        }};
        const auto        result = runProcess(probeSpec({"hang"}), cancel);
        canceller.join();
        then("it is killed and reported as cancelled", [&] {
            require(result.status == ProcessStatus::cancelled);
        });
    });
#ifndef _WIN32
    when("a signal ends it", [&] {
        const auto result = run(probeSpec({"kill-self"}));
        then("the signal is reported", [&] {
            require(result.status == ProcessStatus::signaled);
            require(result.signal == SIGKILL);
        });
    });
#endif
}};

Scenario capturedOutput{"Captured output is bounded and the excess does not block the program", [] {
    auto spec           = probeSpec({"big", "5000000"});
    spec.maxOutputBytes = 1000;
    const auto result   = run(spec);
    then("the program completes, with the first bytes and the truncation flag", [&] {
        require(result.status == ProcessStatus::exited);
        require(result.standardOutput == std::string(1000, 'x'));
        require(result.outputTruncated);
        require(!result.errorTruncated);
    });
}};

Scenario environmentExplicit{"The environment is explicit", [] {
    when("it is not inherited", [&] {
        auto spec = probeSpec({"env", "HOME"});
        then("the program sees nothing of the caller's environment", [&] {
            require(run(spec).standardOutput == "<unset>");
        });
        spec                       = probeSpec({"env", "CHOSEN"});
        spec.environment["CHOSEN"] = "caf\xC3\xA9 value";
        then("only the entries the caller chose", [&] {
            require(run(spec).standardOutput == "caf\xC3\xA9 value");
        });
    });
    when("it is inherited", [&] {
        auto spec               = probeSpec({"envcount"});
        spec.inheritEnvironment = true;
        const auto inherited    = std::stoi(run(spec).standardOutput);
        auto       bare         = probeSpec({"envcount"});
        const auto minimal      = std::stoi(run(bare).standardOutput);
        then("the caller's variables are there", [&] {
            require(inherited > minimal);
        });
    });
}};

Scenario programDescendants{"A program and its descendants end together", [] {
    TempRoot root{"descendants"};
    std::filesystem::create_directories(root.path);
    const auto marker = root.path / "alive.txt";
    when("the timeout kills a program that started a descendant", [&] {
        auto spec         = probeSpec({"spawn-later", marker.generic_string()});
        spec.timeout      = 500ms;
        const auto result = run(spec);
        std::this_thread::sleep_for(3s);
        then("the descendant did not survive to do its work", [&] {
            require(result.status == ProcessStatus::timeout);
            require(!std::filesystem::exists(marker));
        });
    });
    when("the program ends at once and leaves a descendant", [&] {
        const auto result = run(probeSpec({"spawn-quick", marker.generic_string()}));
        std::this_thread::sleep_for(3s);
        then("the descendant is ended with it", [&] {
            require(result.status == ProcessStatus::exited);
            require(!std::filesystem::exists(marker));
        });
    });
}};

Scenario invalidRequests{"Invalid requests are refused before anything starts", [] {
    requireThrows([&] { run(ProcessSpec{}); });
    auto nul = probeSpec({"args", std::string("a\0b", 3)});
    requireThrows([&] { run(nul); });
    auto timeout    = probeSpec({"exit"});
    timeout.timeout = 0ms;
    requireThrows([&] { run(timeout); });
    auto environment               = probeSpec({"exit"});
    environment.environment["A=B"] = "x";
    requireThrows([&] { run(environment); });
}};

Scenario hostConfigurationStrict{"The host configuration is strict", [] {
    using webfront::testing::detail::hostConfiguration;
    const auto absolute = webfront::json::quote((std::filesystem::temp_directory_path() / "x").generic_string());
    requireNothrow([&] { hostConfiguration(R"({"programs":[],"temporary":)" + absolute + "}"); });
    for (const auto* bad : {R"({"programs":["relative"],"temporary":"/tmp/x"})",
                            R"({"programs":[]})",
                            R"({"programs":[],"temporary":"relative"})",
                            R"({"programs":[],"temporary":"/t","shell":true})",
                            R"({"programs":[],"programs":[],"temporary":"/t"})"})
        requireThrows([&] { hostConfiguration(bad); });
}};

Scenario onlyOwner{"Only the owner's programs run, whatever the spec writes", [] {
    TempRoot    root{"authorization"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  request = [](const std::string& program) {
        return "{\"program\":" + webfront::json::quote(program) + ",\"args\":[\"exit\",\"0\"]}";
    };
    then("the authorized program runs", [&] {
        const auto id = host.runStart(request(probe.generic_string()));
        require(field(waitFor(host, id), "status") == "exited");
        host.runRelease(id);
    });
    then("another program, a relative name, a shell or a missing program is refused", [&] {
        requireThrows([&] { (void)host.runStart(request("/bin/sh")); }, contains("not authorized"));
        requireThrows([&] { (void)host.runStart(request("host_probe")); }, contains("not authorized"));
        requireThrows([&] { (void)host.runStart(request("")); }, contains("not authorized"));
        requireThrows([&] { (void)host.runStart(request((probe.parent_path() / "missing").generic_string())); }, contains("not authorized"));
    });
    then("unknown options and out-of-range limits are refused", [&] {
        const auto base = webfront::json::quote(probe.generic_string());
        requireThrows([&] { (void)host.runStart("{\"program\":" + base + ",\"shell\":true}"); });
        requireThrows([&] { (void)host.runStart("{\"program\":" + base + ",\"timeoutMs\":0}"); });
        requireThrows([&] { (void)host.runStart("{\"program\":" + base + ",\"timeoutMs\":3600001}"); });
        requireThrows([&] { (void)host.runStart("{\"program\":" + base + ",\"maxOutputBytes\":1.5}"); });
        requireThrows([&] { (void)host.runStart("{\"program\":" + base + ",\"cwd\":\"/\"}"); });
    });
}};

Scenario programAuthorized{"A program authorized by no configuration is never reachable", [] {
    TempRoot    root{"empty-grant"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {}))};
    requireThrows([&] { (void)host.runStart("{\"program\":" + webfront::json::quote(probe.generic_string()) + "}"); }, contains("not authorized"));
}};

Scenario temporarySpaces{"Temporary spaces are private, confined and removed", [] {
    TempRoot    root{"spaces"};
    std::string first, second;
    {
        HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
        first  = host.makeTemp();
        second = host.makeTemp();
        then("two spaces are distinct directories under the root", [&] {
            require(first != second);
            require(std::filesystem::is_directory(webfront::testing::detail::pathFromUtf8(first)));
            require(std::filesystem::is_directory(webfront::testing::detail::pathFromUtf8(second)));
        });
        then("files are written and read in chunks, including Unicode names and empty files", [&] {
            const auto                file = first + "/caf\xC3\xA9 espace.json";
            std::vector<std::uint8_t> a(HostSession::chunkSize, 'a'), b{'b', 'c'};
            host.writeFile(file, 0, a);
            host.writeFile(file, static_cast<double>(a.size()), b);
            const auto whole = host.readFile(file, 0);
            require(whole.size() == HostSession::chunkSize);
            require(text(host.readFile(file, static_cast<double>(a.size()))) == "bc");
            host.writeFile(first + "/empty", 0, {});
            require(host.readFile(first + "/empty", 0).empty());
            requireThrows([&] { host.writeFile(file, 5, b); });  // A chunk must continue the file.
        });
        then("paths outside the spaces are refused, however they are spelled", [&] {
            requireThrows([&] { host.writeFile(root.path.generic_string() + "/loose", 0, {}); });
            requireThrows([&] { host.writeFile(first + "/../loose", 0, {}); });
            requireThrows([&] { host.writeFile(first + "/../../escape", 0, {}); });
            requireThrows([&] { host.writeFile("relative", 0, {}); });
            requireThrows([&] { (void)host.readFile((std::filesystem::temp_directory_path() / "nothing-here").generic_string(), 0); });
            requireThrows([&] { host.writeFile(first, 0, {}); });  // The space itself is not a file.
        });
#ifndef _WIN32
        then("a link to the outside does not open a way out", [&] {
            std::filesystem::create_directory_symlink(std::filesystem::temp_directory_path(), webfront::testing::detail::pathFromUtf8(first) / "out");
            requireThrows([&] { host.writeFile(first + "/out/webfront-host-escape", 0, {}); });
            require(!std::filesystem::exists(std::filesystem::temp_directory_path() / "webfront-host-escape"));
        });
#endif
        then("a program works in a space and a working directory outside is refused", [&] {
            host.writeFile(first + "/in.txt", 0, {'h', 'i'});
            const auto id      = host.runStart("{\"program\":" + webfront::json::quote(probe.generic_string()) + ",\"args\":[\"cat\","
                                          + webfront::json::quote(first + "/in.txt") + "],\"cwd\":" + webfront::json::quote(first) + "}");
            const auto summary = waitFor(host, id);
            require(field(summary, "status") == "exited");
            require(text(host.runRead(id, 1, 0)) == "hi");
            host.runRelease(id);
            requireThrows([&] { (void)host.runStart("{\"program\":" + webfront::json::quote(probe.generic_string())
                                         + ",\"cwd\":" + webfront::json::quote(std::filesystem::temp_directory_path().generic_string()) + "}"); });
        });
        then("removing a space deletes it, and only a space of this run", [&] {
            host.removeTemp(second);
            require(!std::filesystem::exists(webfront::testing::detail::pathFromUtf8(second)));
            requireThrows([&] { host.removeTemp(second); });
            requireThrows([&] { host.removeTemp(root.path.generic_string()); });
        });
    }
    then("ending the session removes every space left", [&] {
        require(!std::filesystem::exists(webfront::testing::detail::pathFromUtf8(first)));
    });
}};

Scenario twoSessions{"Two sessions on one root keep separate spaces", [] {
    TempRoot    root{"concurrent"};
    HostSession one{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    HostSession two{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  a = one.makeTemp();
    const auto  b = two.makeTemp();
    require(a != b);
    requireThrows([&] { one.writeFile(b + "/x", 0, {}); });
}};

Scenario runsBounded{"Runs are bounded, cancellable and released", [] {
    TempRoot    root{"runs"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  hang = "{\"program\":" + webfront::json::quote(probe.generic_string()) + ",\"args\":[\"hang\"],\"timeoutMs\":60000}";
    then("a cancelled run reports it, and its output stays readable until released", [&] {
        const auto id = host.runStart(hang);
        std::this_thread::sleep_for(100ms);
        require(!host.runPoll(id));
        requireThrows([&] { (void)host.runRead(id, 1, 0); });
        host.runCancel(id);
        require(field(waitFor(host, id), "status") == "cancelled");
        host.runRelease(id);
        requireThrows([&] { (void)host.runPoll(id); });
    });
    then("too many simultaneous runs are refused", [&] {
        std::vector<double> ids;
        for (std::size_t count = 0; count < HostSession::maxActiveRuns; ++count)
            ids.push_back(host.runStart(hang));
        requireThrows([&] { (void)host.runStart(hang); });
        for (const auto id : ids)
            host.runRelease(id);
    });
    then("a session destroyed with a running program kills it", [&] {
        auto owned = std::make_unique<HostSession>(webfront::testing::detail::hostConfiguration(configurationFor(root, {probe})));
        (void)owned->runStart(hang);
        const auto begin = std::chrono::steady_clock::now();
        owned.reset();
        require(std::chrono::steady_clock::now() - begin < 10s);
    });
}};

#ifndef _WIN32
Scenario programStill{"A program is still bounded when the application ignores SIGCHLD", [] {
    const auto previous = std::signal(SIGCHLD, SIG_IGN);
    auto       spec     = probeSpec({"exit", "0"});
    spec.timeout        = 100ms;
    const auto begin    = std::chrono::steady_clock::now();
    const auto result   = run(spec);
    std::signal(SIGCHLD, previous);
    then("the call ends promptly and never reports an exit status it could not read", [&] {
        require(std::chrono::steady_clock::now() - begin < 5s);
        require(result.status != ProcessStatus::launchFailed);
        require((result.status == ProcessStatus::exited ? result.exitCode == 0 || !result.error.empty() : true));
    });
}};

Scenario temporarySpacesClosed{"Temporary spaces are closed to other users", [] {
    TempRoot    root{"private"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  space = webfront::testing::detail::pathFromUtf8(host.makeTemp());
    namespace fs      = std::filesystem;
    for (const auto& path : {root.path, space})
        require((fs::status(path).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
}};

Scenario programCannot{"A program that cannot start through exec is reported, with a reason", [] {
    ProcessSpec spec;
    spec.program      = probe.parent_path();  // A directory: exec fails with EACCES.
    const auto result = run(spec);
    require(result.status == ProcessStatus::launchFailed);
    require(!result.error.empty());
}};
#endif

#ifdef __linux__
Scenario programDoes{"A program does not outlive an application killed without a destructor", [] {
    TempRoot root{"orphan"};
    std::filesystem::create_directories(root.path);
    const auto marker = root.path / "alive.txt";
    const auto child  = ::fork();
    if (child == 0) {  // The application: starts a long program, then is killed like a CTest deadline kills it.
        auto spec    = probeSpec({"write-later", marker.generic_string()});
        spec.timeout = 60s;
        (void)run(spec);
        ::_exit(0);
    }
    require(child > 0);
    std::this_thread::sleep_for(500ms);
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    std::this_thread::sleep_for(3s);
    then("the program was ended with it", [&] {
        require(!std::filesystem::exists(marker));
    });
}};
#endif

Scenario programKeeps{"A program that keeps its pipes full cannot hold off the deadline", [] {
    for (const auto delay : {0, 1000}) {
        when("the child delays its first write by " + std::to_string(delay) + " milliseconds", [&] {
            auto spec           = probeSpec({"flood", std::to_string(delay)});
            spec.timeout        = 300ms;
            spec.maxOutputBytes = 1000;
            const auto begin    = std::chrono::steady_clock::now();
            const auto result   = run(spec);
            then("the timeout is honoured and any captured output stays bounded", [&] {
                require(result.status == ProcessStatus::timeout);
                require(std::chrono::steady_clock::now() - begin < 5s);
                // The deadline includes child startup: a busy runner may kill it before its first write.
                require(result.standardOutput.size() <= spec.maxOutputBytes);
                require(result.standardError.size() <= spec.maxOutputBytes);
            });
        });
    }
}};

Scenario finiteFlood{"A finite flood drains both pipes and truncates each captured stream", [] {
    auto spec           = probeSpec({"flood-finite"});
    spec.maxOutputBytes = 1000;
    const auto result   = run(spec);
    then("both streams reach the limit and the child completes without blocking", [&] {
        require(result.status == ProcessStatus::exited);
        require(result.exitCode == 0);
        require(result.standardOutput == std::string(1000, 'f'));
        require(result.standardError == std::string(1000, 'f'));
        require(result.outputTruncated);
        require(result.errorTruncated);
    });
}};

Scenario authorizedProgram{"An authorized program that is missing cannot start, and is not called unauthorized", [] {
    TempRoot    root{"missing-grant"};
    const auto  absent = probe.parent_path() / "not-built-yet";
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {absent}))};
    const auto  id      = host.runStart("{\"program\":" + webfront::json::quote(absent.generic_string()) + "}");
    const auto  summary = waitFor(host, id);
    then("it is a launch failure with a reason", [&] {
        require(field(summary, "status") == "launchFailed");
        require(!field(summary, "error").empty());
    });
    host.runRelease(id);
    requireThrows([&] { (void)host.runStart("{\"program\":" + webfront::json::quote((absent.parent_path() / "other").generic_string()) + "}"); }, contains("not authorized"));
}};

#ifndef _WIN32
Scenario temporarySpace{"A temporary space that cannot be removed is reported and stays registered", [] {
    if (::geteuid() == 0)
        skip("permissions do not stop root");
    TempRoot    root{"locked"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  name  = host.makeTemp();
    const auto  space = webfront::testing::detail::pathFromUtf8(name);
    namespace fs      = std::filesystem;
    fs::create_directory(space / "locked");
    std::ofstream{space / "locked" / "file"} << "x";
    fs::permissions(space / "locked", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    then("removeTemp fails instead of pretending, and a later attempt can succeed", [&] {
        requireThrows([&] { host.removeTemp(name); }, contains("Cannot remove"));
        require(fs::exists(space));
        fs::permissions(space / "locked", fs::perms::owner_all, fs::perm_options::replace);
        requireNothrow([&] { host.removeTemp(name); });
        require(!fs::exists(space));
    });
}};
#endif

Scenario requestProcess{"A request the process runner would refuse is refused before any run exists", [] {
    TempRoot    root{"early-validation"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  base = "{\"program\":" + webfront::json::quote(probe.generic_string());
    requireThrows([&] { (void)host.runStart(base + ",\"env\":{\"A=B\":\"x\"}}"); });
    requireThrows([&] { (void)host.runStart(base + ",\"env\":{\"\":\"x\"}}"); });
    requireThrows([&] { (void)host.runStart(base + ",\"args\":[\"a\\u0000b\"]}"); });
}};

#ifndef _WIN32
Scenario programStarts{"A program starts with default signal dispositions, whatever the application ignores", [] {
    const auto pipe   = std::signal(SIGPIPE, SIG_IGN);
    const auto child  = std::signal(SIGCHLD, SIG_IGN);
    auto       spec   = probeSpec({"dispositions"});
    spec.timeout      = 5s;
    const auto result = run(spec);
    std::signal(SIGPIPE, pipe);
    std::signal(SIGCHLD, child);
    then("neither is ignored in the program", [&] {
        require(result.status == ProcessStatus::exited);
        require(result.standardOutput == "default default ");
    });
}};
#endif

Scenario fileLarger{"A file larger than the limit is not read", [] {
    TempRoot    root{"large"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  space = host.makeTemp();
    const auto  file  = webfront::testing::detail::pathFromUtf8(space) / "big.bin";
    std::ofstream{file, std::ios::binary} << "x";
    std::filesystem::resize_file(file, HostSession::maxFileBytes + 1);  // Sparse where the system allows it.
    requireThrows([&] { (void)host.readFile(space + "/big.bin", 0); }, contains("64 MiB"));
    std::filesystem::resize_file(file, 10);
    require(host.readFile(space + "/big.bin", 0).size() == 10);
}};

#ifndef _WIN32
Scenario programOutput{"A program's output is captured when the application closed its standard descriptors", [] {
    const auto child = ::fork();
    if (child == 0) {
        ::close(0);
        ::close(1);
        ::close(2);
        const auto result = run(probeSpec({"args", "hello"}));
        ::_exit(result.status == ProcessStatus::exited && result.standardOutput == "[0]hello\n" ? 0 : 1);
    }
    require(child > 0);
    int status = 0;
    ::waitpid(child, &status, 0);
    require(WIFEXITED(status));
    require(WEXITSTATUS(status) == 0);
}};
#endif

#ifdef __linux__
Scenario failedLaunch{"A failed launch is reported as one when the application closed its standard descriptors", [] {
    const auto child = ::fork();
    if (child == 0) {
        ::close(0);
        ::close(1);
        ProcessSpec spec;
        spec.program      = probe.parent_path() / "no-such-program";
        const auto result = run(spec);
        ::_exit(result.status == ProcessStatus::launchFailed && !result.error.empty() ? 0 : 1);
    }
    require(child > 0);
    int status = 0;
    ::waitpid(child, &status, 0);
    require(WIFEXITED(status));
    require(WEXITSTATUS(status) == 0);
}};
#endif
