#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <webfront/testing/Host.hpp>
#include <webfront/testing/Process.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
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
    REQUIRE(at != std::string::npos);
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
    FAIL("the run did not end");
    return {};
}

std::string text(const std::vector<std::uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}
}  // namespace

SCENARIO("A program receives its arguments literally, without a shell") {
    GIVEN("arguments with spaces, quotes, Unicode, shell metacharacters and an empty one") {
        const std::vector<std::string>
             arguments{"two words", "", "a;b|c&d $HOME `id` $(id) *", "\"quoted\"", "back\\slash\\", "caf\xC3\xA9 \xE2\x82\xAC", "--flag=x y"};
        auto spec = probeSpec({"args"});
        spec.arguments.insert(spec.arguments.end(), arguments.begin(), arguments.end());
        const auto result = run(spec);
        THEN("each one arrives as one exact argument") {
            REQUIRE(result.status == ProcessStatus::exited);
            REQUIRE(result.exitCode == 0);
            std::string expected;
            for (std::size_t index = 0; index < arguments.size(); ++index)
                expected += "[" + std::to_string(index) + "]" + arguments[index] + "\n";
            REQUIRE(result.standardOutput == expected);
        }
    }
}

SCENARIO("Every way a program can end has a structured result") {
    WHEN("it exits with a non-zero code") {
        const auto result = run(probeSpec({"exit", "7"}));
        THEN("the code is reported") {
            REQUIRE(result.status == ProcessStatus::exited);
            REQUIRE(result.exitCode == 7);
        }
    }
    WHEN("it writes to both streams") {
        const auto result = run(probeSpec({"streams"}));
        THEN("each stream is captured separately") {
            REQUIRE(result.standardOutput == "to-stdout");
            REQUIRE(result.standardError == "to-stderr");
            REQUIRE(result.exitCode == 3);
        }
    }
    WHEN("the program does not exist") {
        ProcessSpec spec;
        spec.program      = probe.parent_path() / "no-such-program";
        const auto result = run(spec);
        THEN("the launch failure is reported with a reason, not as an exit code") {
            REQUIRE(result.status == ProcessStatus::launchFailed);
            REQUIRE_FALSE(result.error.empty());
        }
    }
    WHEN("it outlives its timeout") {
        auto spec         = probeSpec({"hang"});
        spec.timeout      = 300ms;
        const auto result = run(spec);
        THEN("it is killed and reported as a timeout") {
            REQUIRE(result.status == ProcessStatus::timeout);
            REQUIRE(result.duration < 5s);
        }
    }
    WHEN("the caller cancels it") {
        std::atomic<bool> cancel{false};
        std::thread       canceller{[&] {
            std::this_thread::sleep_for(200ms);
            cancel = true;
        }};
        const auto        result = runProcess(probeSpec({"hang"}), cancel);
        canceller.join();
        THEN("it is killed and reported as cancelled") {
            REQUIRE(result.status == ProcessStatus::cancelled);
        }
    }
#ifndef _WIN32
    WHEN("a signal ends it") {
        const auto result = run(probeSpec({"kill-self"}));
        THEN("the signal is reported") {
            REQUIRE(result.status == ProcessStatus::signaled);
            REQUIRE(result.signal == SIGKILL);
        }
    }
#endif
}

SCENARIO("Captured output is bounded and the excess does not block the program") {
    auto spec           = probeSpec({"big", "5000000"});
    spec.maxOutputBytes = 1000;
    const auto result   = run(spec);
    THEN("the program completes, with the first bytes and the truncation flag") {
        REQUIRE(result.status == ProcessStatus::exited);
        REQUIRE(result.standardOutput == std::string(1000, 'x'));
        REQUIRE(result.outputTruncated);
        REQUIRE_FALSE(result.errorTruncated);
    }
}

SCENARIO("The environment is explicit") {
    WHEN("it is not inherited") {
        auto spec = probeSpec({"env", "HOME"});
        THEN("the program sees nothing of the caller's environment") {
            REQUIRE(run(spec).standardOutput == "<unset>");
        }
        spec                       = probeSpec({"env", "CHOSEN"});
        spec.environment["CHOSEN"] = "caf\xC3\xA9 value";
        THEN("only the entries the caller chose") {
            REQUIRE(run(spec).standardOutput == "caf\xC3\xA9 value");
        }
    }
    WHEN("it is inherited") {
        auto spec               = probeSpec({"envcount"});
        spec.inheritEnvironment = true;
        const auto inherited    = std::stoi(run(spec).standardOutput);
        auto       bare         = probeSpec({"envcount"});
        const auto minimal      = std::stoi(run(bare).standardOutput);
        THEN("the caller's variables are there") {
            REQUIRE(inherited > minimal);
        }
    }
}

SCENARIO("A program and its descendants end together") {
    TempRoot root{"descendants"};
    std::filesystem::create_directories(root.path);
    const auto marker = root.path / "alive.txt";
    WHEN("the timeout kills a program that started a descendant") {
        auto spec         = probeSpec({"spawn-later", marker.generic_string()});
        spec.timeout      = 500ms;
        const auto result = run(spec);
        std::this_thread::sleep_for(3s);
        THEN("the descendant did not survive to do its work") {
            REQUIRE(result.status == ProcessStatus::timeout);
            REQUIRE_FALSE(std::filesystem::exists(marker));
        }
    }
    WHEN("the program ends at once and leaves a descendant") {
        const auto result = run(probeSpec({"spawn-quick", marker.generic_string()}));
        std::this_thread::sleep_for(3s);
        THEN("the descendant is ended with it") {
            REQUIRE(result.status == ProcessStatus::exited);
            REQUIRE_FALSE(std::filesystem::exists(marker));
        }
    }
}

SCENARIO("Invalid requests are refused before anything starts") {
    REQUIRE_THROWS(run(ProcessSpec{}));
    auto nul = probeSpec({"args", std::string("a\0b", 3)});
    REQUIRE_THROWS(run(nul));
    auto timeout    = probeSpec({"exit"});
    timeout.timeout = 0ms;
    REQUIRE_THROWS(run(timeout));
    auto environment               = probeSpec({"exit"});
    environment.environment["A=B"] = "x";
    REQUIRE_THROWS(run(environment));
}

SCENARIO("The host configuration is strict") {
    using webfront::testing::detail::hostConfiguration;
    REQUIRE_NOTHROW(hostConfiguration(R"({"programs":[],"temporary":"/tmp/x"})"));
    for (const auto* bad : {R"({"programs":["relative"],"temporary":"/tmp/x"})",
                            R"({"programs":[]})",
                            R"({"programs":[],"temporary":"relative"})",
                            R"({"programs":[],"temporary":"/t","shell":true})",
                            R"({"programs":[],"programs":[],"temporary":"/t"})"})
        REQUIRE_THROWS(hostConfiguration(bad));
}

SCENARIO("Only the owner's programs run, whatever the spec writes") {
    TempRoot    root{"authorization"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  request = [](const std::string& program) {
        return "{\"program\":" + webfront::json::quote(program) + ",\"args\":[\"exit\",\"0\"]}";
    };
    THEN("the authorized program runs") {
        const auto id = host.runStart(request(probe.generic_string()));
        REQUIRE(field(waitFor(host, id), "status") == "exited");
        host.runRelease(id);
    }
    THEN("another program, a relative name, a shell or a missing program is refused") {
        REQUIRE_THROWS_WITH(host.runStart(request("/bin/sh")), Catch::Matchers::ContainsSubstring("not authorized"));
        REQUIRE_THROWS_WITH(host.runStart(request("host_probe")), Catch::Matchers::ContainsSubstring("not authorized"));
        REQUIRE_THROWS_WITH(host.runStart(request("")), Catch::Matchers::ContainsSubstring("not authorized"));
        REQUIRE_THROWS_WITH(host.runStart(request((probe.parent_path() / "missing").generic_string())), Catch::Matchers::ContainsSubstring("not authorized"));
    }
    THEN("unknown options and out-of-range limits are refused") {
        const auto base = webfront::json::quote(probe.generic_string());
        REQUIRE_THROWS(host.runStart("{\"program\":" + base + ",\"shell\":true}"));
        REQUIRE_THROWS(host.runStart("{\"program\":" + base + ",\"timeoutMs\":0}"));
        REQUIRE_THROWS(host.runStart("{\"program\":" + base + ",\"timeoutMs\":3600001}"));
        REQUIRE_THROWS(host.runStart("{\"program\":" + base + ",\"maxOutputBytes\":1.5}"));
        REQUIRE_THROWS(host.runStart("{\"program\":" + base + ",\"cwd\":\"/\"}"));
    }
}

SCENARIO("A program authorized by no configuration is never reachable") {
    TempRoot    root{"empty-grant"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {}))};
    REQUIRE_THROWS_WITH(host.runStart("{\"program\":" + webfront::json::quote(probe.generic_string()) + "}"),
                        Catch::Matchers::ContainsSubstring("not authorized"));
}

SCENARIO("Temporary spaces are private, confined and removed") {
    TempRoot    root{"spaces"};
    std::string first, second;
    {
        HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
        first  = host.makeTemp();
        second = host.makeTemp();
        THEN("two spaces are distinct directories under the root") {
            REQUIRE(first != second);
            REQUIRE(std::filesystem::is_directory(webfront::testing::detail::pathFromUtf8(first)));
            REQUIRE(std::filesystem::is_directory(webfront::testing::detail::pathFromUtf8(second)));
        }
        THEN("files are written and read in chunks, including Unicode names and empty files") {
            const auto                file = first + "/caf\xC3\xA9 espace.json";
            std::vector<std::uint8_t> a(HostSession::chunkSize, 'a'), b{'b', 'c'};
            host.writeFile(file, 0, a);
            host.writeFile(file, static_cast<double>(a.size()), b);
            const auto whole = host.readFile(file, 0);
            REQUIRE(whole.size() == HostSession::chunkSize);
            REQUIRE(text(host.readFile(file, static_cast<double>(a.size()))) == "bc");
            host.writeFile(first + "/empty", 0, {});
            REQUIRE(host.readFile(first + "/empty", 0).empty());
            REQUIRE_THROWS(host.writeFile(file, 5, b));  // A chunk must continue the file.
        }
        THEN("paths outside the spaces are refused, however they are spelled") {
            REQUIRE_THROWS(host.writeFile(root.path.generic_string() + "/loose", 0, {}));
            REQUIRE_THROWS(host.writeFile(first + "/../loose", 0, {}));
            REQUIRE_THROWS(host.writeFile(first + "/../../escape", 0, {}));
            REQUIRE_THROWS(host.writeFile("relative", 0, {}));
            REQUIRE_THROWS(host.readFile((std::filesystem::temp_directory_path() / "nothing-here").generic_string(), 0));
            REQUIRE_THROWS(host.writeFile(first, 0, {}));  // The space itself is not a file.
        }
#ifndef _WIN32
        THEN("a link to the outside does not open a way out") {
            std::filesystem::create_directory_symlink(std::filesystem::temp_directory_path(), webfront::testing::detail::pathFromUtf8(first) / "out");
            REQUIRE_THROWS(host.writeFile(first + "/out/webfront-host-escape", 0, {}));
            REQUIRE_FALSE(std::filesystem::exists(std::filesystem::temp_directory_path() / "webfront-host-escape"));
        }
#endif
        THEN("a program works in a space and a working directory outside is refused") {
            host.writeFile(first + "/in.txt", 0, {'h', 'i'});
            const auto id      = host.runStart("{\"program\":" + webfront::json::quote(probe.generic_string()) + ",\"args\":[\"cat\","
                                          + webfront::json::quote(first + "/in.txt") + "],\"cwd\":" + webfront::json::quote(first) + "}");
            const auto summary = waitFor(host, id);
            REQUIRE(field(summary, "status") == "exited");
            REQUIRE(text(host.runRead(id, 1, 0)) == "hi");
            host.runRelease(id);
            REQUIRE_THROWS(host.runStart("{\"program\":" + webfront::json::quote(probe.generic_string())
                                         + ",\"cwd\":" + webfront::json::quote(std::filesystem::temp_directory_path().generic_string()) + "}"));
        }
        THEN("removing a space deletes it, and only a space of this run") {
            host.removeTemp(second);
            REQUIRE_FALSE(std::filesystem::exists(webfront::testing::detail::pathFromUtf8(second)));
            REQUIRE_THROWS(host.removeTemp(second));
            REQUIRE_THROWS(host.removeTemp(root.path.generic_string()));
        }
    }
    THEN("ending the session removes every space left") {
        REQUIRE_FALSE(std::filesystem::exists(webfront::testing::detail::pathFromUtf8(first)));
    }
}

SCENARIO("Two sessions on one root keep separate spaces") {
    TempRoot    root{"concurrent"};
    HostSession one{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    HostSession two{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  a = one.makeTemp();
    const auto  b = two.makeTemp();
    REQUIRE(a != b);
    REQUIRE_THROWS(one.writeFile(b + "/x", 0, {}));
}

SCENARIO("Runs are bounded, cancellable and released") {
    TempRoot    root{"runs"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  hang = "{\"program\":" + webfront::json::quote(probe.generic_string()) + ",\"args\":[\"hang\"],\"timeoutMs\":60000}";
    THEN("a cancelled run reports it, and its output stays readable until released") {
        const auto id = host.runStart(hang);
        std::this_thread::sleep_for(100ms);
        REQUIRE_FALSE(host.runPoll(id));
        REQUIRE_THROWS(host.runRead(id, 1, 0));
        host.runCancel(id);
        REQUIRE(field(waitFor(host, id), "status") == "cancelled");
        host.runRelease(id);
        REQUIRE_THROWS(host.runPoll(id));
    }
    THEN("too many simultaneous runs are refused") {
        std::vector<double> ids;
        for (std::size_t count = 0; count < HostSession::maxActiveRuns; ++count)
            ids.push_back(host.runStart(hang));
        REQUIRE_THROWS(host.runStart(hang));
        for (const auto id : ids)
            host.runRelease(id);
    }
    THEN("a session destroyed with a running program kills it") {
        auto owned = std::make_unique<HostSession>(webfront::testing::detail::hostConfiguration(configurationFor(root, {probe})));
        (void)owned->runStart(hang);
        const auto begin = std::chrono::steady_clock::now();
        owned.reset();
        REQUIRE(std::chrono::steady_clock::now() - begin < 10s);
    }
}

#ifndef _WIN32
SCENARIO("A program is still bounded when the application ignores SIGCHLD") {
    const auto previous = std::signal(SIGCHLD, SIG_IGN);
    auto       spec     = probeSpec({"exit", "0"});
    spec.timeout        = 100ms;
    const auto begin    = std::chrono::steady_clock::now();
    const auto result   = run(spec);
    std::signal(SIGCHLD, previous);
    THEN("the call ends promptly and never reports an exit status it could not read") {
        REQUIRE(std::chrono::steady_clock::now() - begin < 5s);
        REQUIRE(result.status != ProcessStatus::launchFailed);
        REQUIRE((result.status == ProcessStatus::exited ? result.exitCode == 0 || !result.error.empty() : true));
    }
}

SCENARIO("Temporary spaces are closed to other users") {
    TempRoot    root{"private"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  space = webfront::testing::detail::pathFromUtf8(host.makeTemp());
    namespace fs      = std::filesystem;
    for (const auto& path : {root.path, space})
        REQUIRE((fs::status(path).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
}

SCENARIO("A program that cannot start through exec is reported, with a reason") {
    ProcessSpec spec;
    spec.program      = probe.parent_path();  // A directory: exec fails with EACCES.
    const auto result = run(spec);
    REQUIRE(result.status == ProcessStatus::launchFailed);
    REQUIRE_FALSE(result.error.empty());
}
#endif

#ifdef __linux__
SCENARIO("A program does not outlive an application killed without a destructor") {
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
    REQUIRE(child > 0);
    std::this_thread::sleep_for(500ms);
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    std::this_thread::sleep_for(3s);
    THEN("the program was ended with it") {
        REQUIRE_FALSE(std::filesystem::exists(marker));
    }
}
#endif

SCENARIO("A program that keeps its pipes full cannot hold off the deadline") {
    auto spec           = probeSpec({"flood"});
    spec.timeout        = 300ms;
    spec.maxOutputBytes = 1000;
    const auto begin    = std::chrono::steady_clock::now();
    const auto result   = run(spec);
    THEN("the timeout is honoured and the output stays bounded") {
        REQUIRE(result.status == ProcessStatus::timeout);
        REQUIRE(std::chrono::steady_clock::now() - begin < 5s);
        REQUIRE(result.standardOutput.size() == 1000);
        REQUIRE(result.standardError.size() == 1000);
    }
}

SCENARIO("An authorized program that is missing cannot start, and is not called unauthorized") {
    TempRoot    root{"missing-grant"};
    const auto  absent = probe.parent_path() / "not-built-yet";
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {absent}))};
    const auto  id      = host.runStart("{\"program\":" + webfront::json::quote(absent.generic_string()) + "}");
    const auto  summary = waitFor(host, id);
    THEN("it is a launch failure with a reason") {
        REQUIRE(field(summary, "status") == "launchFailed");
        REQUIRE_FALSE(field(summary, "error").empty());
    }
    host.runRelease(id);
    REQUIRE_THROWS_WITH(host.runStart("{\"program\":" + webfront::json::quote((absent.parent_path() / "other").generic_string()) + "}"),
                        Catch::Matchers::ContainsSubstring("not authorized"));
}

#ifndef _WIN32
SCENARIO("A temporary space that cannot be removed is reported and stays registered") {
    if (::geteuid() == 0)
        SKIP("permissions do not stop root");
    TempRoot    root{"locked"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  name  = host.makeTemp();
    const auto  space = webfront::testing::detail::pathFromUtf8(name);
    namespace fs      = std::filesystem;
    fs::create_directory(space / "locked");
    std::ofstream{space / "locked" / "file"} << "x";
    fs::permissions(space / "locked", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    THEN("removeTemp fails instead of pretending, and a later attempt can succeed") {
        REQUIRE_THROWS_WITH(host.removeTemp(name), Catch::Matchers::ContainsSubstring("Cannot remove"));
        REQUIRE(fs::exists(space));
        fs::permissions(space / "locked", fs::perms::owner_all, fs::perm_options::replace);
        REQUIRE_NOTHROW(host.removeTemp(name));
        REQUIRE_FALSE(fs::exists(space));
    }
}
#endif

SCENARIO("A request the process runner would refuse is refused before any run exists") {
    TempRoot    root{"early-validation"};
    HostSession host{webfront::testing::detail::hostConfiguration(configurationFor(root, {probe}))};
    const auto  base = "{\"program\":" + webfront::json::quote(probe.generic_string());
    REQUIRE_THROWS(host.runStart(base + ",\"env\":{\"A=B\":\"x\"}}"));
    REQUIRE_THROWS(host.runStart(base + ",\"env\":{\"\":\"x\"}}"));
    REQUIRE_THROWS(host.runStart(base + ",\"args\":[\"a\\u0000b\"]}"));
}

#ifndef _WIN32
SCENARIO("A program starts with default signal dispositions, whatever the application ignores") {
    const auto pipe   = std::signal(SIGPIPE, SIG_IGN);
    const auto child  = std::signal(SIGCHLD, SIG_IGN);
    auto       spec   = probeSpec({"dispositions"});
    spec.timeout      = 5s;
    const auto result = run(spec);
    std::signal(SIGPIPE, pipe);
    std::signal(SIGCHLD, child);
    THEN("neither is ignored in the program") {
        REQUIRE(result.status == ProcessStatus::exited);
        REQUIRE(result.standardOutput == "default default ");
    }
}
#endif
